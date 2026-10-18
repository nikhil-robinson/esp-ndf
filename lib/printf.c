/* printf family: %d %i %u %x %X %p %c %s %f %e %g %%, flags '-' '0' '+' ' ',
 * width and precision (or *), 'l'/'ll'/'z'/'h' accepted (ll = 64-bit).
 * One formatter, three sinks: console (line buffered), string (snprintf).
 * Floats are formatted from the double's bits with integer arithmetic only.
 * ponytail: |x| >= 2^64 prints in %e from a 64-bit approximation (17 digits
 * correct is not guaranteed there); values below ~1e-18 print as 0 in %f. */
#include "console.h"
#include <string.h>

typedef struct { char *buf; size_t cap; size_t n; int total; bool console; char line[128]; } out_t;

static void flush(out_t *o) { console_write(o->line, (int)o->n); o->n = 0; }

static void put(out_t *o, char c)
{
    o->total++;
    if (o->console) {
        o->line[o->n++] = c;
        if (o->n == sizeof o->line || c == '\n') flush(o);
    } else if (o->n + 1 < o->cap) o->buf[o->n++] = c;
}

static void pad(out_t *o, int count, char c) { while (count-- > 0) put(o, c); }

/* decimal digits of x: integer digits + up to nf fraction digits (truncated) */
typedef struct { char d[64]; int ni, nd, sh; bool neg, inf, nan; } dec_t;   /* value = digits * 10^sh */

static void to_dec(double x, int nf, dec_t *r)
{
    union { double d; uint64_t u; } v = { x };
    r->neg = v.u >> 63; r->inf = r->nan = false; r->ni = r->nd = r->sh = 0;
    int e = (int)((v.u >> 52) & 0x7FF);
    uint64_t m = v.u & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) { if (m) r->nan = true; else r->inf = true; return; }
    if (e) m |= 1ull << 52; else e = 1;
    if (!m) { r->d[0] = 0; return; }                             /* zero: no digits */
    e -= 1075;                                                   /* x = m * 2^e */
    uint64_t ip, fr = 0; int fb = 0;                             /* fraction = fr / 2^fb */
    if (e >= 0) {
        if (e > 11) {                                            /* beyond 2^64: scale down by 10s */
            int k = 0;                                           /* x ~= m * 10^k, m kept below 2^61 */
            while (e > 0) {
                if (m >> 60) { m /= 10; k++; } else { m <<= 1; e--; }
            }
            ip = m;
            char t[24]; int n = 0; do t[n++] = '0' + ip % 10; while (ip /= 10);
            while (n) r->d[r->ni++] = t[--n];
            r->sh = k;
            r->d[r->ni] = 0;
            return;
        }
        ip = m << e;
    } else {
        int s = -e;
        while (s > 60) {                                          /* below 2^-60: scale up by 10s */
            if (m < (1ull << 59)) { m *= 10; r->sh--; } else { m >>= 1; s--; }
        }
        ip = s < 64 ? m >> s : 0;
        fr = m & ((1ull << s) - 1); fb = s;
    }
    char t[24]; int n = 0; do t[n++] = '0' + ip % 10; while (ip /= 10);
    while (n) r->d[r->ni++] = t[--n];
    if (r->ni == 1 && r->d[0] == '0') r->ni = 0;                 /* no leading zero digit */
    if (r->sh) nf = 60;                                          /* tiny values: need the leading zeros' digits */
    while (r->nd < nf && r->ni + r->nd < 63) {
        fr *= 10;
        r->d[r->ni + r->nd++] = '0' + (char)(fr >> fb);
        fr &= (1ull << fb) - 1;
    }
    r->d[r->ni + r->nd] = fr ? 1 : 0;                            /* sticky marker (not a digit) */
}

/* round digit string d[0..n) to keep k digits, half to even on exact ties
 * (sticky: nonzero bits beyond d[n)); returns 1 on carry out of d[0] */
static int round_at(char *d, int n, int k, bool sticky)
{
    if (k >= n || k < 0) return 0;
    int up = d[k] > '5';
    if (d[k] == '5') {
        bool rest = sticky;
        for (int i = k + 1; i < n && !rest; i++) rest = d[i] != '0';
        up = rest || (k > 0 ? (d[k - 1] - '0') & 1 : 0);
    }
    for (int i = k; i < n; i++) d[i] = '0';
    for (int i = k - 1; up && i >= 0; i--) { if (d[i] == '9') d[i] = '0'; else { d[i]++; up = 0; } }
    return up;
}

static void fmt_float(out_t *o, double x, char conv, int prec, int width, bool left, char padc, char plus)
{
    if (prec < 0) prec = 6;
    if (prec > 40) prec = 40;
    dec_t r; to_dec(x, (conv == 'f' || conv == 'F') ? prec + 1 : 60, &r);   /* %f: one rounding digit + sticky */
    char out[400]; int n = 0;
    char sign = r.neg ? '-' : plus;
    if (r.inf || r.nan) {
        const char *s = r.nan ? "nan" : "inf";
        if (conv == 'F' || conv == 'E' || conv == 'G') s = r.nan ? "NAN" : "INF";
        memcpy(out, s, 3); n = 3; padc = ' ';
    } else {
        int total = r.ni + r.nd;
        bool sticky = r.d[total] != 0;
        bool g = conv == 'g' || conv == 'G', up = conv == 'E' || conv == 'G';
        int P = g ? (prec ? prec : 1) : prec;
        /* exponent after rounding to P (g) or prec+1 (e) significant digits */
        char dd[64]; memcpy(dd, r.d, 64);
        int first = 0; while (first < total && dd[first] == '0') first++;
        int x10 = 0;
        if (first < total) {
            x10 = r.ni - first - 1 + r.sh;
            int carry = round_at(dd, total, first + (g ? P : prec + 1), sticky);
            int nf = 0; while (nf < total && dd[nf] == '0') nf++;
            if (carry) { x10++; dd[0] = '1'; nf = 0; memset(dd + 1, '0', 62); total = 63; first = 0; }   /* 9.99 -> 10.0 */
            else if (nf < first) { x10++; first = nf; }
        }
        bool use_e = conv == 'e' || conv == 'E' || (g && (x10 < -4 || x10 >= P));
        if (use_e) {
            int nfrac = g ? P - 1 : prec;
            out[n++] = first < total ? dd[first] : '0';
            int fn = 0; char fbuf[48];
            for (int i = 1; i <= nfrac && fn < 47; i++) fbuf[fn++] = first + i < total ? dd[first + i] : '0';
            if (g) while (fn && fbuf[fn - 1] == '0') fn--;
            if (fn) { out[n++] = '.'; memcpy(out + n, fbuf, fn); n += fn; }
            out[n++] = up ? 'E' : 'e';
            int ex = x10; out[n++] = ex < 0 ? '-' : '+'; if (ex < 0) ex = -ex;
            if (ex >= 100) out[n++] = '0' + ex / 100;
            out[n++] = '0' + ex / 10 % 10; out[n++] = '0' + ex % 10;
        } else {
            int nfrac = g ? P - 1 - x10 : prec;
            if (nfrac < 0) nfrac = 0;
            memcpy(dd, r.d, 64); total = r.ni + r.nd;              /* round again at the fixed position */
            int ni = r.ni;
            if (r.sh < 0) {                                        /* tiny: move the point left */
                int z = -r.sh - ni;                                /* zeros after the point */
                if (z >= 63) { total = 0; ni = 0; }
                else { int keep = total < 63 - z ? total : 63 - z; memmove(dd + z, dd, keep); memset(dd, '0', z); total = z + keep; ni = 0; }
            }
            int tot = total;
            if (round_at(dd, total, ni + nfrac, sticky)) { memmove(dd + 1, dd, 62); dd[0] = '1'; ni++; tot++; }
            if (!ni) out[n++] = '0';
            for (int i = 0; i < ni && n < 80; i++) out[n++] = dd[i];
            for (int i = 0; i < r.sh && n < 360; i++) out[n++] = '0';   /* huge: digits below 10^sh are zero */
            int fn = 0; char fbuf[48];
            for (int i = 0; i < nfrac && fn < 47; i++) fbuf[fn++] = ni + i < tot ? dd[ni + i] : '0';
            if (g) while (fn && fbuf[fn - 1] == '0') fn--;
            if (fn) { out[n++] = '.'; memcpy(out + n, fbuf, fn); n += fn; }
        }
    }
    int fill = width - n - (sign ? 1 : 0);
    if (sign && padc == '0') put(o, sign);
    if (!left) pad(o, fill, padc);
    if (sign && padc != '0') put(o, sign);
    for (int i = 0; i < n; i++) put(o, out[i]);
    if (left) pad(o, fill, ' ');
}

static int format(out_t *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { put(o, *f); continue; }
        f++;
        bool left = false; char padc = ' ', plus = 0; int width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') padc = '0';
            else if (*f == '+') plus = '+';
            else if (*f == ' ') { if (!plus) plus = ' '; }
            else if (*f == '#') {}
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; if (width < 0) { left = true; width = -width; } }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'z' || *f == 'h' || *f == 'j' || *f == 't') { if (*f == 'l' || *f == 'j') lng++; f++; }
        if (left) padc = ' ';

        char tmp[24], *s = tmp + sizeof tmp;
        const char *str = s;
        char sign = 0;
        uint64_t v;
        int len = -1;
        switch (*f) {
        case 'c': *--s = (char)va_arg(ap, int); str = s; len = 1; break;
        case 's': str = va_arg(ap, const char *); if (!str) str = "(null)";
                  for (len = 0; str[len] && (prec < 0 || len < prec); len++) {}
                  padc = ' '; break;
        case 'd': case 'i': {
            int64_t x = lng > 1 ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            if (x < 0) sign = '-'; else sign = plus;
            v = x < 0 ? -(uint64_t)x : (uint64_t)x;
            goto dec;
        }
        case 'u':
            v = lng > 1 ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
        dec:
            if (v || prec != 0) do *--s = '0' + v % 10; while (v /= 10);
            str = s; break;
        case 'p': padc = '0'; width = 8; /* fallthrough */
        case 'x': case 'X': {
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            v = lng > 1 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            if (v || prec != 0) do *--s = dig[v & 15]; while (v >>= 4);
            str = s; break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_float(o, va_arg(ap, double), *f, prec, width, left, padc, plus);
            continue;
        case '%': put(o, '%'); continue;
        case 0: return o->total;
        default:  put(o, '%'); put(o, *f); continue;
        }
        if (len < 0) len = (int)(tmp + sizeof tmp - str);
        int zeros = 0;
        if (prec >= 0 && *f != 's' && *f != 'c') { if (prec > len) zeros = prec - len; padc = ' '; }
        int fill = width - len - zeros - (sign ? 1 : 0);
        if (sign && padc == '0') put(o, sign);
        if (!left) pad(o, fill, padc);
        if (sign && padc != '0') put(o, sign);
        pad(o, zeros, '0');
        for (int i = 0; i < len; i++) put(o, str[i]);
        if (left) pad(o, fill, ' ');
    }
    return o->total;
}

int vprintf(const char *f, va_list ap)
{
    out_t o = { .console = true };
    format(&o, f, ap);
    if (o.n) flush(&o);
    return o.total;
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r;
}

int vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    out_t o = { .buf = buf, .cap = cap };
    format(&o, f, ap);
    if (cap) buf[o.n] = 0;
    return o.total;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); va_end(ap); return r;
}

int puts(const char *s) { int n = printf("%s\n", s); return n; }
int putchar(int c) { char ch = (char)c; console_write(&ch, 1); return c; }
