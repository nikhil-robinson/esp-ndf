/* IDF sibling of the time part of components/lib/test_apps/libc_test (newlib time/gmtime_r/mktime/strftime). */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_cpu.h"

void app_main(void)
{
    struct timeval tv = { .tv_sec = 1761350400 }; settimeofday(&tv, 0);
    setenv("TZ", "UTC0", 1); tzset();
    (void)time(0);
    uint32_t c0 = esp_cpu_get_cycle_count(); time_t now = time(0); uint32_t tc = esp_cpu_get_cycle_count() - c0;
    struct tm m; c0 = esp_cpu_get_cycle_count(); gmtime_r(&now, &m); uint32_t gc = esp_cpu_get_cycle_count() - c0;
    struct tm mm = m; c0 = esp_cpu_get_cycle_count(); (void)mktime(&mm); uint32_t mc = esp_cpu_get_cycle_count() - c0;
    char b[64]; c0 = esp_cpu_get_cycle_count(); strftime(b, sizeof b, "%F %T", &m); uint32_t sc = esp_cpu_get_cycle_count() - c0;
    printf("idf_time: time() %lu, gmtime_r %lu, mktime %lu, strftime %lu cycles (%s)\n", tc, gc, mc, sc, b);
    printf("idf_time done\n");
}
