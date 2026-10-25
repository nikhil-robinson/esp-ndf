/* printf family: %d %i %u %x %X %p %c %s %f %e %g %%, flags '-' '0' '+' ' ',
 * width and precision (or *), 'l'/'ll'/'z'/'h' accepted (ll = 64-bit).
 * One formatter, three sinks: console (line buffered), string (snprintf).
 * Floats: exact decimal digits from the double's bits (integer arithmetic,
 * big integers when needed), correctly rounded at any precision. */
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

/* ---- floats ----------------------------------------------------------------
 * Exact digits: value * 10^k = D (an integer, as a digit string) + a fraction,
 * sticky = fraction != 0. Rounding is then done on the decimal string (half
 * to even on exact ties), so every result is the correctly rounded one, at
 * any precision. Digits come from 64-bit arithmetic when that is exact
 * (2^-60 <= |x| < 2^64, <= 60 digits), else from big integers (components/lib/bigint.c),
 * in a separate frame (~2 KB of stack) used only for those values. */
#define KMAX 1100                  /* fraction digits beyond this are 0 for any double */

typedef struct { char *d; int n, k; bool sticky; } dig_t;   /* value = d[0..n) * 10^-k (+ sticky) */

/* fast: x = m * 2^e with -60 <= e <= 11, k <= 60 */
static void digits64(uint64_t m, int e, int k, dig_t *r)
{
    uint64_t ip = e >= 0 ? m << e : m >> -e, fr = e >= 0 ? 0 : m & ((1ull << -e) - 1);
    int fb = e >= 0 ? 0 : -e;
    char t[24]; int n = 0;
    if (ip) do t[n++] = '0' + ip % 10; while (ip /= 10);
    r->n = 0;
    while (n) r->d[r->n++] = t[--n];
    for (int i = 0; i < k; i++) {
        fr *= 10;                                     /* fr < 2^60: no overflow */
        char c = '0' + (char)(fr >> fb);
        fr &= (1ull << fb) - 1;
        if (r->n || c != '0') r->d[r->n++] = c;      /* no leading zeros */
    }
    r->k = k; r->sticky = fr != 0;
}

#include "bigint.h"
static void digits_big(uint64_t m, int e, int k, dig_t *r)
{
    big_t b;
    big_set_u64(&b, m);
    big_mul_pow10(&b, k);
    r->sticky = false;
    if (e >= 0) big_shl(&b, e); else r->sticky = big_shr(&b, -e);
    /* decimal digits, 9 at a time from the low end, written backwards */
    int pos = 0;
    char *end = r->d + KMAX + 330;
    while (b.n) {
        uint32_t c = big_divmod_1e9(&b);
        for (int i = 0; i < 9; i++) { *--end = '0' + c % 10; c /= 10; pos++; }
    }
    while (pos && *end == '0') { end++; pos--; }   /* leading zeros of the top chunk */
    memmove(r->d, end, pos);
    r->n = pos; r->k = k;
}

/* Round to `keep` leading digits, half to even (sticky: nonzero beyond d[n)).
 * d[0] sits at decimal exponent *x10. keep <= 0 rounds at or above d[0]. */
static void round_digits(char *d, int *n, int *x10, int keep, bool sticky)
{
    if (keep >= *n) return;
    if (keep < 0) { *n = 0; return; }                 /* below half a unit: 0 */
    int up = d[keep] > '5';
    if (d[keep] == '5') {
        bool rest = sticky;
        for (int i = keep + 1; i < *n && !rest; i++) rest = d[i] != '0';
        up = rest || (keep > 0 && ((d[keep - 1] - '0') & 1));
    }
    *n = keep;
    if (!up) return;
    int i = keep - 1;
    while (i >= 0 && d[i] == '9') d[i--] = '0';
    if (i >= 0) { d[i]++; return; }
    d[0] = '1';                                       /* carried out of the top (or keep = 0): 10^(x10+1) */
    if (!keep) *n = 1;
    (*x10)++;
}

typedef struct { out_t *o; char conv, padc, plus; int prec, width; bool left, neg, alt; } ffmt_t;

static void emit_float(ffmt_t *f, dig_t *r)
{
    out_t *o = f->o;
    bool g = f->conv == 'g' || f->conv == 'G', upc = f->conv == 'E' || f->conv == 'G';
    char *d = r->d; int n = r->n;
    int x10 = n - r->k - 1;                           /* decimal exponent of d[0] */
    bool use_e = f->conv == 'e' || f->conv == 'E';
    int frac;
    if (g) {                                          /* P significant digits, style from the rounded exponent */
        int P = f->prec ? f->prec : 1;
        round_digits(d, &n, &x10, P, r->sticky);
        if (!n) x10 = 0;
        use_e = x10 < -4 || x10 >= P;
        frac = use_e ? P - 1 : P - 1 - x10;
    } else {
        frac = f->prec;
        round_digits(d, &n, &x10, use_e ? frac + 1 : x10 + 1 + frac, r->sticky);
        if (!n) x10 = use_e ? 0 : x10;
    }
    #define DIG(t) (x10 - (t) >= 0 && x10 - (t) < n ? d[x10 - (t)] : '0')   /* digit at 10^t */
    int ip_top = use_e ? (n ? x10 : 0) : (x10 > 0 ? x10 : 0);   /* exponents of the integer digits */
    int ip_bot = use_e ? ip_top : 0;
    int nfrac = frac;
    if (g && !f->alt) while (nfrac > 0 && DIG(ip_bot - nfrac) == '0') nfrac--;   /* %g: no trailing zeros (unless #) */
    char ex[8]; int en = 0;
    if (use_e) {
        int e10 = n ? x10 : 0;
        ex[en++] = upc ? 'E' : 'e';
        ex[en++] = e10 < 0 ? '-' : '+';
        if (e10 < 0) e10 = -e10;
        if (e10 >= 100) ex[en++] = '0' + e10 / 100;
        ex[en++] = '0' + e10 / 10 % 10;
        ex[en++] = '0' + e10 % 10;
    }
    bool point = nfrac || f->alt;                     /* # keeps the point */
    int body = ip_top - ip_bot + 1 + (point ? 1 + nfrac : 0) + en;
    char sign = f->neg ? '-' : f->plus;
    int fill = f->width - body - (sign ? 1 : 0);
    if (sign && f->padc == '0') put(o, sign);
    if (!f->left) pad(o, fill, f->padc);
    if (sign && f->padc != '0') put(o, sign);
    for (int t = ip_top; t >= ip_bot; t--) put(o, DIG(t));
    if (point) {
        put(o, '.');
        for (int t = ip_bot - 1; t >= ip_bot - nfrac; t--) put(o, DIG(t));
    }
    for (int i = 0; i < en; i++) put(o, ex[i]);
    if (f->left) pad(o, fill, ' ');
    #undef DIG
}

static void __attribute__((noinline)) fmt_big(ffmt_t *f, uint64_t m, int e, int k)
{
    char buf[KMAX + 340];
    dig_t r = { .d = buf };
    digits_big(m, e, k, &r);
    emit_float(f, &r);
}

static void fmt_float(out_t *o, double x, char conv, int prec, int width, bool left, char padc, char plus, bool alt)
{
    union { double d; uint64_t u; } v = { x };
    ffmt_t f = { o, conv, left ? ' ' : padc, plus, prec < 0 ? 6 : prec, width, left, (v.u >> 63) != 0, alt };
    int be = (int)((v.u >> 52) & 0x7FF);
    uint64_t m = v.u & 0xFFFFFFFFFFFFFull;
    if (be == 0x7FF) {
        const char *s = m ? "nan" : "inf";
        if (conv == 'F' || conv == 'E' || conv == 'G') s = m ? "NAN" : "INF";
        char sign = f.neg ? '-' : plus;
        int fill = width - 3 - (sign ? 1 : 0);
        if (!left) pad(o, fill, ' ');
        if (sign) put(o, sign);
        for (int i = 0; i < 3; i++) put(o, s[i]);
        if (left) pad(o, fill, ' ');
        return;
    }
    if (be) m |= 1ull << 52; else be = 1;
    int e = be - 1075;                                /* |x| = m * 2^e */
    int P = f.prec > KMAX ? KMAX : f.prec;            /* digits past KMAX are exactly 0 */
    int k;
    if (conv == 'f' || conv == 'F') k = P + 1;
    else {                                            /* through P+1 significant digits */
        int lead = m ? e + 63 - __builtin_clzll(m) : 0;           /* binary exponent of the leading bit */
        int x10 = (lead * 77) >> 8;                   /* ~ lead * log10(2), within 1 */
        if (lead < 0) x10 -= 1;
        k = P + 2 - x10;
        if (k < 0) k = 0;
        if (k > KMAX + 1) k = KMAX + 1;
    }
    if (m && e >= -60 && e <= 11 && k <= 60) {
        char buf[64 + 24];
        dig_t r = { .d = buf };
        digits64(m, e, k, &r);
        emit_float(&f, &r);
    } else if (!m) {
        char buf[1];
        dig_t r = { .d = buf, .n = 0, .k = k };
        emit_float(&f, &r);
    } else fmt_big(&f, m, e, k);
}

/* %a: [-]0xh.hhhp+d, exact by default (trailing zeros dropped), else rounded
 * to `prec` hex digits half to even (a carry makes the lead digit 2, as glibc). */
static void fmt_hexfloat(out_t *o, double x, bool upper, int prec, int width, bool left, char padc, char plus)
{
    union { double d; uint64_t u; } v = { x };
    const char *hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = v.u >> 63 ? '-' : plus;
    int be = (int)((v.u >> 52) & 0x7FF);
    uint64_t fr = v.u & 0xFFFFFFFFFFFFFull;
    if (be == 0x7FF) { fmt_float(o, x, upper ? 'F' : 'f', 0, width, left, ' ', plus, false); return; }
    int lead = be ? 1 : 0, e = be ? be - 1023 : (fr ? -1022 : 0);
    int nd = 13;                                       /* hex digits of the fraction */
    if (prec >= 0 && prec < 13) {
        int drop = 4 * (13 - prec);
        uint64_t keep = fr >> drop, rem = fr & ((1ull << drop) - 1), half = 1ull << (drop - 1);
        int odd = prec ? (int)(keep & 1) : lead & 1;     /* parity of the last kept digit */
        if (rem > half || (rem == half && odd)) keep++;
        if (keep >> (4 * prec)) { lead++; keep = 0; }   /* carried into the lead digit */
        fr = keep << drop;
        nd = prec;
    } else if (prec < 0) {
        while (nd && !((fr >> (4 * (13 - nd))) & 15)) nd--;
    }
    int pn = prec > 13 ? prec : nd;                    /* digits printed (zeros past 13) */
    char ex[8]; int en = 0, ee = e;
    ex[en++] = upper ? 'P' : 'p';
    ex[en++] = ee < 0 ? '-' : '+';
    if (ee < 0) ee = -ee;
    char t[6]; int tn = 0;
    do t[tn++] = '0' + ee % 10; while (ee /= 10);
    while (tn) ex[en++] = t[--tn];
    int body = 3 + (pn ? 1 + pn : 0) + en;
    int fill = width - body - (sign ? 1 : 0);
    if (left) padc = ' ';
    if (sign && padc == '0') put(o, sign);
    if (padc == '0') { put(o, '0'); put(o, upper ? 'X' : 'x'); }
    if (!left) pad(o, fill, padc);
    if (sign && padc != '0') put(o, sign);
    if (padc != '0') { put(o, '0'); put(o, upper ? 'X' : 'x'); }
    put(o, hx[lead]);
    if (pn) {
        put(o, '.');
        for (int i = 0; i < pn; i++) put(o, i < 13 ? hx[(fr >> (48 - 4 * i)) & 15] : '0');
    }
    for (int i = 0; i < en; i++) put(o, ex[i]);
    if (left) pad(o, fill, ' ');
}

static int format(out_t *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { put(o, *f); continue; }
        f++;
        bool left = false, alt = false; char padc = ' ', plus = 0; int width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') padc = '0';
            else if (*f == '+') plus = '+';
            else if (*f == ' ') { if (!plus) plus = ' '; }
            else if (*f == '#') alt = true;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; if (width < 0) { left = true; width = -width; } }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int sh = 0;                                   /* h count: hh -> char, h -> short */
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'j' || *f == 'q') lng = 2;     /* intmax_t: 64-bit */
            else if (*f == 'h') sh++;
            else if (*f != 'z' && *f != 't' && *f != 'L') break;   /* size_t/ptrdiff_t 32-bit, long double = double */
        }
        if (left) padc = ' ';

        char tmp[24], *s = tmp + sizeof tmp;
        const char *str = s;
        char sign = 0;
        const char *pfx = 0;                          /* "0x" for %#x */
        uint64_t v;
        int len = -1;
        switch (*f) {
        case 'c': *--s = (char)va_arg(ap, int); str = s; len = 1; break;
        case 's': str = va_arg(ap, const char *); if (!str) str = "(null)";
                  for (len = 0; str[len] && (prec < 0 || len < prec); len++) {}
                  padc = ' '; break;
        case 'd': case 'i': {
            int64_t x = lng > 1 ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            if (sh == 1) x = (int16_t)x; else if (sh > 1) x = (int8_t)x;
            if (x < 0) sign = '-'; else sign = plus;
            v = x < 0 ? -(uint64_t)x : (uint64_t)x;
            goto dec;
        }
        case 'u':
            v = lng > 1 ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            if (sh == 1) v = (uint16_t)v; else if (sh > 1) v = (uint8_t)v;
        dec:
            if (v || prec != 0) do *--s = '0' + v % 10; while (v /= 10);
            str = s; break;
        case 'p':
            v = (uintptr_t)va_arg(ap, void *);
            do *--s = "0123456789abcdef"[v & 15]; while (v >>= 4);
            *--s = 'x'; *--s = '0';
            str = s; break;
        case 'x': case 'X': {
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            v = lng > 1 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            if (sh == 1) v = (uint16_t)v; else if (sh > 1) v = (uint8_t)v;
            bool nz = v != 0;
            if (v || prec != 0) do *--s = dig[v & 15]; while (v >>= 4);
            if (alt && nz) pfx = *f == 'X' ? "0X" : "0x";
            str = s; break;
        }
        case 'o':
            v = lng > 1 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            if (sh == 1) v = (uint16_t)v; else if (sh > 1) v = (uint8_t)v;
            if (v || prec != 0) do *--s = '0' + (v & 7); while (v >>= 3);
            if (alt && *s != '0') *--s = '0';     /* # : leading 0 */
            str = s; break;
        case 'n':                                 /* writes memory: off unless NDF_PRINTF_N (format-string attacks) */
            if (!NDF_PRINTF_N) { (void)va_arg(ap, int *); continue; }
            if (lng > 1) *va_arg(ap, int64_t *) = o->total;
            else if (sh == 1) *va_arg(ap, short *) = (short)o->total;
            else if (sh > 1) *va_arg(ap, signed char *) = (signed char)o->total;
            else *va_arg(ap, int *) = o->total;
            continue;
        case 'a': case 'A':
            fmt_hexfloat(o, va_arg(ap, double), *f == 'A', prec, width, left, padc, plus);
            continue;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_float(o, va_arg(ap, double), *f, prec, width, left, padc, plus, alt);
            continue;
        case '%': put(o, '%'); continue;
        case 0: return o->total;
        default:  put(o, '%'); put(o, *f); continue;
        }
        if (len < 0) len = (int)(tmp + sizeof tmp - str);
        int zeros = 0;
        if (prec >= 0 && *f != 's' && *f != 'c') { if (prec > len) zeros = prec - len; padc = ' '; }
        int fill = width - len - zeros - (sign ? 1 : 0) - (pfx ? 2 : 0);
        if (sign && padc == '0') put(o, sign);
        if (pfx && padc == '0') { put(o, pfx[0]); put(o, pfx[1]); }
        if (!left) pad(o, fill, padc);
        if (sign && padc != '0') put(o, sign);
        if (pfx && padc != '0') { put(o, pfx[0]); put(o, pfx[1]); }
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
