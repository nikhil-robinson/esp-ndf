/* Small printf: %d %i %u %x %X %p %c %s %%, width, '0' and '-' flags,
 * 'l'/'ll'/'z' accepted (ll prints 64-bit). Buffers a line, then writes. */
#include "console.h"

typedef struct { char buf[128]; int n, total; } out_t;

static void flush(out_t *o) { console_write(o->buf, o->n); o->n = 0; }

static void put(out_t *o, char c)
{
    o->buf[o->n++] = c;
    o->total++;
    if (o->n == (int)sizeof o->buf || c == '\n') flush(o);
}

static void pad(out_t *o, int count, char c) { while (count-- > 0) put(o, c); }

int vprintf(const char *f, va_list ap)
{
    out_t o = { .n = 0, .total = 0 };
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        bool left = false; char padc = ' '; int width = 0, lng = 0;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') padc = '0';
            else break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z') { if (*f == 'l') lng++; f++; }

        char tmp[24], *s = tmp + sizeof tmp;
        const char *str = s;
        bool neg = false;
        uint64_t v;
        switch (*f) {
        case 'c': *--s = (char)va_arg(ap, int); str = s; break;
        case 's': str = va_arg(ap, const char *); if (!str) str = "(null)"; s = 0; break;
        case 'd': case 'i': {
            int64_t x = lng > 1 ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            neg = x < 0; v = neg ? -(uint64_t)x : (uint64_t)x;
            do *--s = '0' + v % 10; while (v /= 10);
            str = s; break;
        }
        case 'u':
            v = lng > 1 ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            do *--s = '0' + v % 10; while (v /= 10);
            str = s; break;
        case 'p': padc = '0'; width = 8; /* fallthrough */
        case 'x': case 'X': {
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            v = lng > 1 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            do *--s = dig[v & 15]; while (v >>= 4);
            str = s; break;
        }
        case '%': put(&o, '%'); continue;
        default:  put(&o, '%'); put(&o, *f); continue;
        }
        int len = 0;
        while ((s == 0 || str + len < tmp + sizeof tmp) && str[len]) len++;
        int fill = width - len - neg;
        if (neg && padc == '0') put(&o, '-');
        if (!left) pad(&o, fill, padc);
        if (neg && padc != '0') put(&o, '-');
        for (int i = 0; i < len; i++) put(&o, str[i]);
        if (left) pad(&o, fill, ' ');
    }
    if (o.n) flush(&o);
    return o.total;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
