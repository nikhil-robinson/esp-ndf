/* int8 NN layers, see nn.h. Arithmetic follows TFLite's reference int8 kernels (as ESP-NN's _ansi code) so results
 * are bit-exact; speed comes from (a) folding the input offset into a per-channel bias, (b) PIE int8 dot products
 * on im2col rows, (c) lookup tables where inputs are int8 (add, softmax exp), (d) vector max/min (pool, relu6). */
#include "nn.h"
#include <string.h>

/* ---- fixed-point helpers (TFLite / gemmlowp definitions) ---- */
static inline int32_t sat_rdhm(int32_t a, int32_t b)                /* saturating rounding doubling high multiply */
{
    if (a == b && a == INT32_MIN) return INT32_MAX;
    int64_t ab = (int64_t)a * b, nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
    int64_t v = ab + nudge;
    return (int32_t)((v + (v < 0 ? (1ll << 31) - 1 : 0)) >> 31);   /* truncating divide by 2^31 */
}
static inline int32_t rdiv_pow2(int32_t x, int e)                   /* rounding divide by 2^e, ties away from zero */
{
    int32_t mask = (1 << e) - 1, rem = x & mask, th = (mask >> 1) + (x < 0);
    return (x >> e) + (rem > th);
}
/* floor((x m + 2^30) / 2^31) equals TFLite's sign-dependent nudge + truncation for every x (m != INT32_MIN):
 * one mull/mulsh pair, no branches (ESP-NN's asm uses the same identity) */
static inline int32_t rdhm(int32_t x, int32_t m) { return (int32_t)(((int64_t)x * m + (1ll << 30)) >> 31); }
static inline int32_t rq(int32_t x, int32_t mult, int32_t shift)
{
    if (shift > 0) return rdhm(x * (1 << shift), mult);
    return rdiv_pow2(rdhm(x, mult), -shift);
}
int32_t nn_requant(int32_t x, int32_t mult, int32_t shift) { return rq(x, mult, shift); }
static inline int8_t clamp8(int32_t v, int32_t lo, int32_t hi) { return (int8_t)(v < lo ? lo : v > hi ? hi : v); }

#if SOC_CPU_HAS_PIE
int32_t nn_dot_s8(const int8_t *a, const int8_t *b, int n16);       /* lib/nn_s8.S: a any alignment, b 16-aligned */
#endif
static int32_t dot8(const int8_t *a, const int8_t *b, int n)
{
    int32_t s = 0; int i = 0;
#if SOC_CPU_HAS_PIE
    if (n >= 16 && !((uintptr_t)b & 15)) { s = nn_dot_s8(a, b, n >> 4); i = n & ~15; }
#endif
    for (; i < n; i++) s += a[i] * b[i];
    return s;
}
static int32_t sum8(const int8_t *a, int n) { int32_t s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }

/* ---- convolution: per output pixel an im2col row (padded taps = -in_off, so (x + in_off) = 0 there), then per
 * output channel acc = dot(row, filter) + in_off * sum(filter) + bias ---- */
#define NN_MAX_CH  512
#define NN_MAX_COL 4608                                             /* fh * fw * in_ch */
static int32_t beff[NN_MAX_CH];
typedef struct { int32_t bias, mult, rsh, rhalf, negm; } nn_chq_t;  /* per-channel requant table (nn_s8.S layout) */
static struct { int32_t out_off, act_min, act_max, fast, pad; nn_chq_t ch[NN_MAX_CH]; } chq;
#if SOC_CPU_HAS_PIE
void nn_conv_row_s8(const int8_t *row, int k16, const int8_t *w, int oc, const nn_chq_t *q, int8_t *out);
void nn_dw_acc16(const int8_t *const *taps, int ntaps, const int8_t *f, int fstride, uint8_t *raw);
void nn_rq_s8(const int32_t *acc, int n, const nn_chq_t *q, int8_t *out, int32_t out_off);
void nn_dw_rq16(const uint8_t *raw, const nn_chq_t *q, int8_t *out, int32_t out_off);
void nn_dw_acc16_i(const int8_t *base, int ch, int row_bytes, const int8_t *f, uint8_t *raw);
int32_t nn_fc_dot_s8(const int8_t *in, const int8_t *w, int n32, const int8_t *splat16);
void nn_relu6_v(int8_t *x, int n16);
void nn_vmax16(const int8_t *const *taps, int ntaps, int8_t *out, const int8_t *lohi32);
void nn_add_lut(const int8_t *a, const int8_t *b, const int32_t *ta, const int32_t *tb, int n, const int32_t *k);
void nn_avg16(const uint8_t *raw, int8_t *out, int k, const int32_t *lohi);
void nn_avg4_16(const int8_t *p0, const int8_t *p1, const int8_t *p2, const int8_t *p3, int8_t *out, const int8_t *cfg);
void nn_max4_16(const int8_t *p0, const int8_t *p1, const int8_t *p2, const int8_t *p3, int8_t *out, const int8_t *cfg);
/* 2x2, stride 2, no padding, 16-channel groups: one PIE kernel call per 16 outputs */
static bool pool2x2(const int8_t *in, int iw, int ih, int8_t *out, int ow, int oh, int sw, int sh, int fw, int fh,
                    int pw, int ph, int32_t act_min, int32_t act_max, int ch, bool avg)
{
    if (fw != 2 || fh != 2 || sw != 2 || sh != 2 || pw || ph || (ch & 15) || (((uintptr_t)in | (uintptr_t)out) & 15) ||
        2 * ow > iw || 2 * oh > ih) return false;
    static int8_t cfg[64] __attribute__((aligned(16)));
    memset(cfg, act_min, 16); memset(cfg + 16, act_max, 16);
    for (int i = 0; i < 8; i++) { ((int16_t *)(cfg + 32))[i] = 2; ((int16_t *)(cfg + 48))[i] = 1; }
    int rs = iw * ch;
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++) {
            const int8_t *p = in + (2 * oy * iw + 2 * ox) * ch;
            for (int c = 0; c < ch; c += 16, out += 16)
                (avg ? nn_avg4_16 : nn_max4_16)(p + c, p + c + ch, p + c + rs, p + c + rs + ch, out, cfg);
        }
    return true;
}
static inline int32_t lane20(const uint8_t *r, int j)              /* QACC lane j of a 20-byte half (20-bit, LE) */
{
    int b = (j * 5) >> 1, sh = (j & 1) * 4;
    uint32_t v = (uint32_t)r[b] | r[b + 1] << 8 | (uint32_t)r[b + 2] << 16;
    return (int32_t)((v >> sh) << 12) >> 12;
}
#endif
static bool chq_build(int oc, const int32_t *b, const int32_t *mult, const int32_t *shift, int32_t oo, int32_t lo, int32_t hi)
{
    chq.out_off = oo; chq.act_min = lo; chq.act_max = hi; chq.fast = lo == -128 && hi == 127;
    for (int o = 0; o < oc; o++) {
        int rs = -shift[o];
        if (rs < 0 || rs > 31) return false;                        /* left shifts: generic C path */
        if (!rs) chq.fast = 0;
        chq.ch[o] = (nn_chq_t){ b[o], mult[o], rs, rs ? 1 << (rs - 1) : 0, rs ? -1 : 0 };
    }
    return true;
}
static int8_t col[NN_MAX_COL] __attribute__((aligned(16)));
static int8_t wpad[1] __attribute__((aligned(16)));

void nn_conv_s8(const nn_dims_t *id, const int8_t *in, int fh, int fw, const int8_t *f, const int32_t *bias,
                const nn_dims_t *od, int8_t *out, const nn_conv_t *p, const int32_t *mult, const int32_t *shift)
{
    int ic = id->c, oc = od->c, k = fh * fw * ic;
    if (oc > NN_MAX_CH || k > NN_MAX_COL) return;
    (void)wpad;
    for (int o = 0; o < oc; o++) beff[o] = (bias ? bias[o] : 0) + p->in_off * sum8(f + o * k, k);
#if SOC_CPU_HAS_PIE
    bool fast = !(k & 15) && !((uintptr_t)f & 15) && chq_build(oc, beff, mult, shift, p->out_off, p->act_min, p->act_max);
#endif
    bool direct = fh == 1 && fw == 1 && !p->pad_w && !p->pad_h;    /* 1x1: the input pixel is the row */
    for (int oy = 0; oy < od->h; oy++)
        for (int ox = 0; ox < od->w; ox++) {
            int by = oy * p->stride_h - p->pad_h, bx = ox * p->stride_w - p->pad_w;
            const int8_t *row;
            if (direct) {
                row = in + (by * id->w + bx) * ic;
#if SOC_CPU_HAS_PIE
                if (fast && ((uintptr_t)row & 15)) { memcpy(col, row, ic); row = col; }
#endif
            }
            else {
                int8_t *c = col;
                bool words = !(ic & 3) && !((uintptr_t)in & 3);
                if (words && by >= 0 && bx >= 0 && by + fh <= id->h && bx + fw <= id->w) {   /* interior: fh runs of fw*ic */
                    int run = fw * ic / 4;
                    uint32_t *d = (uint32_t *)c;
                    for (int y = 0; y < fh; y++, d += run) {
                        const uint32_t *q = (const uint32_t *)(in + ((by + y) * id->w + bx) * ic);
                        for (int i = 0; i < run; i++) d[i] = q[i];
                    }
                    row = col;
                    goto have_row;
                }
                uint32_t padw = (uint8_t)-p->in_off * 0x01010101u;
                for (int y = 0; y < fh; y++)
                    for (int x = 0; x < fw; x++, c += ic) {
                        int iy = by + y, ix = bx + x;
                        bool pad = iy < 0 || iy >= id->h || ix < 0 || ix >= id->w;
                        if (ic == 16 && words) {                     /* the common case: fixed-size copies, inlined */
                            if (pad) { uint32_t *d = (uint32_t *)c; d[0] = d[1] = d[2] = d[3] = padw; }
                            else { uint32_t *d = (uint32_t *)c; const uint32_t *q = (const uint32_t *)(in + (iy * id->w + ix) * 16); d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3]; }
                        } else if (words) {                          /* inline word copies: no call per tap */
                            uint32_t *d = (uint32_t *)c; const uint32_t *sp = (const uint32_t *)(in + (iy * id->w + ix) * ic);
                            if (pad) for (int i = 0; i < ic / 4; i++) d[i] = padw;
                            else for (int i = 0; i < ic / 4; i++) d[i] = sp[i];
                        } else if (pad) memset(c, -p->in_off, ic);
                        else memcpy(c, in + (iy * id->w + ix) * ic, ic);
                    }
                row = col;
            }
        have_row:
#if SOC_CPU_HAS_PIE
            if (fast) { nn_conv_row_s8(row, k >> 4, f, oc, chq.ch, out); out += oc; continue; }
#endif
            for (int o = 0; o < oc; o++) {
                int32_t acc = dot8(row, f + o * k, k) + beff[o];
                acc = rq(acc, mult[o], shift[o]) + p->out_off;
                *out++ = clamp8(acc, p->act_min, p->act_max);
            }
        }
}

/* ---- depthwise (channel multiplier 1): per channel 9 MACs; offset folded per channel for full windows ---- */
void nn_dwconv_s8(const nn_dims_t *id, const int8_t *in, int fh, int fw, const int8_t *f, const int32_t *bias,
                  const nn_dims_t *od, int8_t *out, const nn_conv_t *p, const int32_t *mult, const int32_t *shift)
{
    int ch = id->c;
#if SOC_CPU_HAS_PIE
    if (!(ch & 15) && !((uintptr_t)in & 15) && !((uintptr_t)f & 15) && fh * fw <= 25 && ch <= NN_MAX_CH) {
        int k = fh * fw;
        for (int c = 0; c < ch; c++) { int32_t sf = 0; for (int t = 0; t < k; t++) sf += f[t * ch + c]; beff[c] = (bias ? bias[c] : 0) + p->in_off * sf; }
        if (chq_build(ch, beff, mult, shift, p->out_off, p->act_min, p->act_max) && chq.fast) {
            static int8_t padv[16] __attribute__((aligned(16)));
            static uint8_t raw[64] __attribute__((aligned(16)));
            const int8_t *taps[25];
            memset(padv, -p->in_off, 16);                             /* padded taps: (x + in_off) = 0 */
            for (int oy = 0; oy < od->h; oy++)
                for (int ox = 0; ox < od->w; ox++) {
                    int by = oy * p->stride_h - p->pad_h, bx = ox * p->stride_w - p->pad_w;
                    bool inner = fh == 3 && fw == 3 && by >= 0 && bx >= 0 && by + 3 <= id->h && bx + 3 <= id->w;
                    for (int c = 0; c < ch; c += 16) {
                        if (inner) {                                     /* no bounds checks, taps by address arithmetic */
                            nn_dw_acc16_i(in + (by * id->w + bx) * ch + c, ch, id->w * ch, f + c, raw);
                            nn_dw_rq16(raw, chq.ch + c, out, p->out_off);
                            out += 16;
                            continue;
                        }
                        for (int y = 0, t = 0; y < fh; y++)
                            for (int x = 0; x < fw; x++, t++) {
                                int iy = by + y, ix = bx + x;
                                taps[t] = iy < 0 || iy >= id->h || ix < 0 || ix >= id->w ? padv : in + (iy * id->w + ix) * ch + c;
                            }
                        nn_dw_acc16(taps, k, f + c, ch, raw);
                        nn_dw_rq16(raw, chq.ch + c, out, p->out_off);
                        out += 16;
                    }
                }
            return;
        }
    }
#endif
    for (int oy = 0; oy < od->h; oy++)
        for (int ox = 0; ox < od->w; ox++) {
            int by = oy * p->stride_h - p->pad_h, bx = ox * p->stride_w - p->pad_w;
            int y0 = by < 0 ? -by : 0, x0 = bx < 0 ? -bx : 0;
            int y1 = id->h - by < fh ? id->h - by : fh, x1 = id->w - bx < fw ? id->w - bx : fw;
            for (int c = 0; c < ch; c++) {
                int32_t acc = bias ? bias[c] : 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                        acc += (in[((by + y) * id->w + bx + x) * ch + c] + p->in_off) * f[(y * fw + x) * ch + c];
                acc = rq(acc, mult[c], shift[c]) + p->out_off;
                *out++ = clamp8(acc, p->act_min, p->act_max);
            }
        }
}

/* ---- fully connected: (w + w_off)(x + in_off) = w.x + in_off sum(w) + w_off sum(x) + n w_off in_off ---- */
void nn_fc_s8(const int8_t *in, int32_t in_off, int len, const int8_t *w, int32_t w_off, const int32_t *bias, int8_t *out,
              int out_n, int32_t out_off, int32_t shift, int32_t mult, int32_t act_min, int32_t act_max)
{
    int32_t sx = w_off ? sum8(in, len) : 0;
#if SOC_CPU_HAS_PIE
    /* in_off * sum(row) as a second PIE dot against a vector of in_off (int8; 128 = 2 x 64) */
    static int8_t splat[NN_MAX_COL] __attribute__((aligned(16)));
    static int32_t splat_v = 1000;
    int sv = in_off == 128 ? 64 : in_off, sm = in_off == 128 ? 2 : 1;
    bool pie = !(len & 15) && len <= NN_MAX_COL && !((uintptr_t)w & 15) && sv >= -128 && sv <= 127;
    if (pie && splat_v != sv) { memset(splat, sv, sizeof splat); splat_v = sv; }
#endif
    for (int o = 0; o < out_n; o++) {
        const int8_t *r = w + o * len;
        int32_t acc;
#if SOC_CPU_HAS_PIE
        if (pie && sm == 1 && !(len & 31) && !((uintptr_t)in & 15)) acc = nn_fc_dot_s8(in, r, len >> 5, splat);
        else if (pie) acc = nn_dot_s8(in, r, len >> 4) + sm * nn_dot_s8(splat, r, len >> 4);
        else
#endif
        acc = dot8(in, r, len) + in_off * sum8(r, len);
        acc += w_off * sx + len * w_off * in_off + (bias ? bias[o] : 0);
        acc = rq(acc, mult, shift) + out_off;
        out[o] = clamp8(acc, act_min, act_max);
    }
}

/* ---- pooling ---- */
void nn_maxpool_s8(const int8_t *in, int iw, int ih, int8_t *out, int ow, int oh, int sw, int sh, int fw, int fh,
                   int pw, int ph, int32_t act_min, int32_t act_max, int ch)
{
#if SOC_CPU_HAS_PIE
    if (pool2x2(in, iw, ih, out, ow, oh, sw, sh, fw, fh, pw, ph, act_min, act_max, ch, false)) return;
    if (!(ch & 15) && !((uintptr_t)in & 15) && !((uintptr_t)out & 15) && fw * fh <= 64) {
        static int8_t lohi[32] __attribute__((aligned(16)));
        memset(lohi, act_min, 16); memset(lohi + 16, act_max, 16);
        const int8_t *taps[64];
        for (int oy = 0; oy < oh; oy++)
            for (int ox = 0; ox < ow; ox++) {
                int by = oy * sh - ph, bx = ox * sw - pw;
                int y0 = by < 0 ? -by : 0, x0 = bx < 0 ? -bx : 0, y1 = ih - by < fh ? ih - by : fh, x1 = iw - bx < fw ? iw - bx : fw;
                for (int c = 0; c < ch; c += 16, out += 16) {
                    int t = 0;
                    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) taps[t++] = in + ((by + y) * iw + bx + x) * ch + c;
                    if (t) nn_vmax16(taps, t, out, lohi); else memset(out, act_min, 16);
                }
            }
        return;
    }
#endif
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++) {
            int by = oy * sh - ph, bx = ox * sw - pw;
            int y0 = by < 0 ? -by : 0, x0 = bx < 0 ? -bx : 0, y1 = ih - by < fh ? ih - by : fh, x1 = iw - bx < fw ? iw - bx : fw;
            for (int c = 0; c < ch; c++) {
                int32_t m = -128;
                for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { int v = in[((by + y) * iw + bx + x) * ch + c]; if (v > m) m = v; }
                *out++ = clamp8(m, act_min, act_max);
            }
        }
}
void nn_avgpool_s8(const int8_t *in, int iw, int ih, int8_t *out, int ow, int oh, int sw, int sh, int fw, int fh,
                   int pw, int ph, int32_t act_min, int32_t act_max, int ch)
{
#if SOC_CPU_HAS_PIE
    if (pool2x2(in, iw, ih, out, ow, oh, sw, sh, fw, fh, pw, ph, act_min, act_max, ch, true)) return;
    if (!(ch & 15) && !((uintptr_t)in & 15) && fw * fh <= 25) {      /* QACC sums against a ones vector */
        static int8_t ones[16] __attribute__((aligned(16))) = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        static uint8_t raw[64] __attribute__((aligned(16)));
        int32_t lohi[2] = { act_min, act_max };
        const int8_t *taps[25];
        for (int oy = 0; oy < oh; oy++)
            for (int ox = 0; ox < ow; ox++) {
                int by = oy * sh - ph, bx = ox * sw - pw;
                int y0 = by < 0 ? -by : 0, x0 = bx < 0 ? -bx : 0, y1 = ih - by < fh ? ih - by : fh, x1 = iw - bx < fw ? iw - bx : fw;
                int cnt = (y1 - y0) * (x1 - x0), k = __builtin_ctz(cnt ? cnt : 1);
                if (!cnt || (cnt & (cnt - 1))) {                     /* not a power of two (borders, 3x3): scalar */
                    for (int c = 0; c < ch; c++) {
                        int32_t sm = 0;
                        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) sm += in[((by + y) * iw + bx + x) * ch + c];
                        sm = sm > 0 ? (sm + cnt / 2) / cnt : (sm - cnt / 2) / (cnt ? cnt : 1);
                        *out++ = clamp8(sm, act_min, act_max);
                    }
                    continue;
                }
                for (int c = 0; c < ch; c += 16, out += 16) {
                    int t = 0;
                    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) taps[t++] = in + ((by + y) * iw + bx + x) * ch + c;
                    nn_dw_acc16(taps, t, ones, 0, raw);
                    nn_avg16(raw, out, k, lohi);
                }
            }
        return;
    }
#endif
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++) {
            int by = oy * sh - ph, bx = ox * sw - pw;
            int y0 = by < 0 ? -by : 0, x0 = bx < 0 ? -bx : 0, y1 = ih - by < fh ? ih - by : fh, x1 = iw - bx < fw ? iw - bx : fw;
            int cnt = (y1 - y0) * (x1 - x0);
            for (int c = 0; c < ch; c++) {
                int32_t s = 0;
                for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) s += in[((by + y) * iw + bx + x) * ch + c];
                s = s > 0 ? (s + cnt / 2) / cnt : (s - cnt / 2) / cnt;
                *out++ = clamp8(s, act_min, act_max);
            }
        }
}

/* ---- elementwise: the input rescale depends on one int8 only -> 256-entry tables ---- */
void nn_add_s8(const int8_t *a, const int8_t *b, int32_t a_off, int32_t b_off, int32_t a_mult, int32_t b_mult,
               int32_t a_shift, int32_t b_shift, int32_t left_shift, int8_t *out, int32_t out_off, int32_t out_mult,
               int32_t out_shift, int32_t act_min, int32_t act_max, int n)
{
    static int32_t ta[256], tb[256];
    for (int v = -128; v < 128; v++) {
        ta[v + 128] = rdiv_pow2(sat_rdhm((v + a_off) * (1 << left_shift), a_mult), -a_shift);
        tb[v + 128] = rdiv_pow2(sat_rdhm((v + b_off) * (1 << left_shift), b_mult), -b_shift);
    }
#if SOC_CPU_HAS_PIE
    int rs = -out_shift;
    if (rs >= 1 && rs <= 31 && act_min == -128 && act_max == 127) {
        static int32_t ua[256], ub[256];                             /* indexed by the raw byte */
        for (int v = 0; v < 256; v++) { ua[v] = ta[(int8_t)v + 128]; ub[v] = tb[(int8_t)v + 128]; }
        int32_t k[8] = { (int32_t)(uintptr_t)out, out_mult, rs, 1 << (rs - 1), -1, out_off, act_min, act_max };
        nn_add_lut(a, b, ua, ub, n, k);
        return;
    }
#endif
    for (int i = 0; i < n; i++) {
        int32_t s = ta[a[i] + 128] + tb[b[i] + 128];
        s = rdiv_pow2(rdhm(s, out_mult), -out_shift) + out_off;
        out[i] = clamp8(s, act_min, act_max);
    }
}
void nn_mul_s8(const int8_t *a, const int8_t *b, int32_t a_off, int32_t b_off, int8_t *out, int32_t out_off,
               int32_t out_mult, int32_t out_shift, int32_t act_min, int32_t act_max, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t s = (a[i] + a_off) * (b[i] + b_off);
        s = rq(s, out_mult, out_shift) + out_off;
        out[i] = clamp8(s, act_min, act_max);
    }
}
void nn_relu6_s8(int8_t *x, int n)
{
    int i = 0;
#if SOC_CPU_HAS_PIE
    if (!((uintptr_t)x & 15)) { nn_relu6_v(x, n >> 4); i = n & ~15; }
#endif
    for (; i < n; i++) x[i] = x[i] < 0 ? 0 : x[i] > 6 ? 6 : x[i];
}

/* ---- softmax (TFLite int8): exp of (x - max) depends only on the int8 difference -> cached table ---- */
static inline int32_t mul_pow2_sat(int32_t v, int e)
{
    int32_t th = (1 << (31 - e)) - 1;
    return v > th ? INT32_MAX : v < -th ? INT32_MIN : v * (1 << e);
}
static int32_t one_over_one_plus_x(int32_t a)                     /* 1 / (1 + a), a in [0, 1) Q0.31, Newton */
{
    int64_t s = (int64_t)a + INT32_MAX;
    int32_t hd = (int32_t)((s + (s >= 0 ? 1 : -1)) / 2);
    int32_t x = 1515870810 + sat_rdhm(hd, -1010580540);
    for (int i = 0; i < 3; i++) x += mul_pow2_sat(sat_rdhm(x, (1 << 29) - sat_rdhm(hd, x)), 2);
    return mul_pow2_sat(x, 1);
}
static int32_t exp_neg(int32_t a)                                 /* exp(a), a <= 0 in Q5.26 -> Q0.31 */
{
    int32_t q = (a & ((1 << 24) - 1)) - (1 << 24), rem = q - a;
    int32_t x = (q << 5) + (1 << 28), x2 = sat_rdhm(x, x), x3 = sat_rdhm(x2, x), x4 = sat_rdhm(x2, x2);
    int32_t x4q = rdiv_pow2(x4, 2), poly = rdiv_pow2(sat_rdhm(x4q + x3, 715827883) + x2, 1);
    int32_t r = 1895147668 + sat_rdhm(1895147668, x + poly);
    static const int32_t k[7] = { 1672461947, 1302514674, 790015084, 290630308, 39332535, 720401, 242 };
    for (int i = 0; i < 7; i++) if (rem & (1 << (24 + i))) r = sat_rdhm(r, k[i]);
    return a ? r : INT32_MAX;
}
void nn_softmax_s8(const int8_t *in, int rows, int cols, int32_t mult, int32_t shift, int32_t diff_min, int8_t *out)
{
    static int32_t tab[256], tab_mult, tab_shift; static uint8_t have[256];
    if (tab_mult != mult || tab_shift != shift) { memset(have, 0, sizeof have); tab_mult = mult; tab_shift = shift; }
    for (int r = 0; r < rows; r++, in += cols, out += cols) {
        int mx = in[0];
        for (int c = 1; c < cols; c++) if (in[c] > mx) mx = in[c];
        int32_t sum = 0;
        for (int c = 0; c < cols; c++) {
            int d = in[c] - mx;
            if (d < diff_min) continue;
            if (!have[-d]) { tab[-d] = exp_neg(sat_rdhm(d * (1 << shift), mult)); have[-d] = 1; }
            sum += rdiv_pow2(tab[-d], 12);
        }
        int hr = __builtin_clz((uint32_t)sum);
        int32_t scale = one_over_one_plus_x((int32_t)(((uint32_t)sum << hr) - (1u << 31)));
        int bits = 12 - hr + 31 - 8;
        for (int c = 0; c < cols; c++) {
            int d = in[c] - mx;
            if (d < diff_min) { out[c] = -128; continue; }
            int32_t v = rdiv_pow2(sat_rdhm(scale, tab[-d]), bits) - 128;
            out[c] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
}
