/* Calendar time: time / gettimeofday / settimeofday / clock_gettime on the 52-bit SYSTIMER (time_us), UTC only
 * (no time-zone database: localtime = gmtime). Civil-date math: H. Hinnant's days_from_civil / civil_from_days,
 * exact over the whole int64 range of time_t. */
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "sys.h"
#include <stdint.h>
#include <stdbool.h>

static int64_t wall_off_us;                       /* wall clock = time_us() + offset; 0 = 1970-01-01 at boot */
static int64_t sec_base;                          /* time(): the current second, recomputed once per second */
static time_t sec_now;
static bool sec_ok;

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    int64_t t = (int64_t)time_us() + wall_off_us;
    if (tv) { tv->tv_sec = t / 1000000; tv->tv_usec = t % 1000000; if (tv->tv_usec < 0) { tv->tv_sec--; tv->tv_usec += 1000000; } }
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (tv) wall_off_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - (int64_t)time_us();
    sec_ok = false;
    return 0;
}

time_t time(time_t *t)
{
    int64_t us = (int64_t)time_us() + wall_off_us;
    if (!sec_ok || us < sec_base || us - sec_base >= 1000000) {   /* new second: one 64-bit divide */
        sec_now = (time_t)(us >= 0 ? us / 1000000 : -((-us + 999999) / 1000000));
        sec_base = (int64_t)sec_now * 1000000;
        sec_ok = true;
    }
    if (t) *t = sec_now;
    return sec_now;
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    int64_t us = (int64_t)time_us() + (id == CLOCK_MONOTONIC ? 0 : wall_off_us);
    ts->tv_sec = us / 1000000; ts->tv_nsec = (long)(us % 1000000) * 1000;
    if (ts->tv_nsec < 0) { ts->tv_sec--; ts->tv_nsec += 1000000000; }
    return 0;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

struct tm *gmtime_r(const time_t *tp, struct tm *r)
{
    int64_t t = *tp, days = t / 86400, sec = t % 86400;
    if (sec < 0) { sec += 86400; days--; }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    memset(r, 0, sizeof *r);
    r->tm_year = (int)(y - 1900); r->tm_mon = (int)m - 1; r->tm_mday = (int)d;
    r->tm_hour = (int)(sec / 3600); r->tm_min = (int)(sec / 60 % 60); r->tm_sec = (int)(sec % 60);
    r->tm_wday = (int)((days % 7 + 11) % 7);               /* 1970-01-01 was a Thursday (4) */
    r->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    return r;
}

struct tm *gmtime(const time_t *t) { static struct tm r; return gmtime_r(t, &r); }
struct tm *localtime_r(const time_t *t, struct tm *r) { return gmtime_r(t, r); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t timegm(struct tm *tm)
{
    /* normalises out-of-range fields like mktime (month first, then the rest in seconds) */
    int64_t mon = tm->tm_mon, y = tm->tm_year + 1900LL + (mon >= 0 ? mon / 12 : -((11 - mon) / 12));
    mon = ((mon % 12) + 12) % 12;
    int64_t t = days_from_civil(y, (unsigned)mon + 1, 1) * 86400 + (int64_t)(tm->tm_mday - 1) * 86400 +
                (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    time_t r = (time_t)t;
    gmtime_r(&r, tm);
    return r;
}

time_t mktime(struct tm *tm) { return timegm(tm); }

static const char *const wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static size_t put(char *s, size_t max, size_t k, const char *v)
{
    while (*v) { if (k + 1 >= max) return (size_t)-1; s[k++] = *v++; }
    return k;
}

static size_t num(char *s, size_t max, size_t k, long v, int w)
{
    char b[24]; int n = 0; bool neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u || n < w);
    if (neg) b[n++] = '-';
    char o[25]; for (int i = 0; i < n; i++) o[i] = b[n - 1 - i];
    o[n] = 0;
    return put(s, max, k, o);
}

/* %Y %m %d %H %M %S %j %a %A? %b %y %e %F %T %s %u %w %% (C locale, the subset logs and file stamps use) */
size_t strftime(char *s, size_t max, const char *f, const struct tm *t)
{
    size_t k = 0;
    for (; *f && k != (size_t)-1; f++) {
        if (*f != '%') { char c[2] = { *f, 0 }; k = put(s, max, k, c); continue; }
        switch (*++f) {
        case 'Y': k = num(s, max, k, t->tm_year + 1900L, 4); break;
        case 'y': k = num(s, max, k, (t->tm_year + 1900L) % 100, 2); break;
        case 'm': k = num(s, max, k, t->tm_mon + 1, 2); break;
        case 'd': k = num(s, max, k, t->tm_mday, 2); break;
        case 'e': { char c[3] = { t->tm_mday < 10 ? ' ' : (char)('0' + t->tm_mday / 10), (char)('0' + t->tm_mday % 10), 0 }; k = put(s, max, k, c); break; }
        case 'H': k = num(s, max, k, t->tm_hour, 2); break;
        case 'M': k = num(s, max, k, t->tm_min, 2); break;
        case 'S': k = num(s, max, k, t->tm_sec, 2); break;
        case 'j': k = num(s, max, k, t->tm_yday + 1, 3); break;
        case 'u': k = num(s, max, k, t->tm_wday ? t->tm_wday : 7, 1); break;
        case 'w': k = num(s, max, k, t->tm_wday, 1); break;
        case 'a': k = put(s, max, k, wd[t->tm_wday % 7]); break;
        case 'b': k = put(s, max, k, mn[t->tm_mon % 12]); break;
        case 'F':
            k = num(s, max, k, t->tm_year + 1900L, 4);
            if (k != (size_t)-1) k = put(s, max, k, "-");
            if (k != (size_t)-1) k = num(s, max, k, t->tm_mon + 1, 2);
            if (k != (size_t)-1) k = put(s, max, k, "-");
            if (k != (size_t)-1) k = num(s, max, k, t->tm_mday, 2);
            break;
        case 'T':
            k = num(s, max, k, t->tm_hour, 2);
            if (k != (size_t)-1) k = put(s, max, k, ":");
            if (k != (size_t)-1) k = num(s, max, k, t->tm_min, 2);
            if (k != (size_t)-1) k = put(s, max, k, ":");
            if (k != (size_t)-1) k = num(s, max, k, t->tm_sec, 2);
            break;
        case 's': { struct tm c = *t; k = num(s, max, k, (long)timegm(&c), 1); break; }
        case '%': k = put(s, max, k, "%"); break;
        default: return 0;
        }
    }
    if (k == (size_t)-1 || k >= max) return 0;
    s[k] = 0;
    return k;
}
