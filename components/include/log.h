/* Logging: LOGE/LOGW/LOGI/LOGD(tag, fmt, ...) -> "I 12.345 tag: message".
 * Levels above NDF_LOG_LEVEL compile to nothing (no code, no strings).
 * NDF_ASSERT(cond): on failure prints file:line and the condition, then halts
 * with interrupts masked (a debugger sees the exact state). */
#pragma once
#include "ndf.h"
#include "sys.h"
#include "console.h"

#define NDF_LOG_(lvl, c, tag, fmt, ...) do { if (NDF_LOG_LEVEL >= (lvl)) { uint32_t ms_ = time_ms(); \
    printf(c " %lu.%03lu %s: " fmt "\n", (unsigned long)(ms_ / 1000), (unsigned long)(ms_ % 1000), tag, ##__VA_ARGS__); } } while (0)
#define LOGE(tag, fmt, ...) NDF_LOG_(NDF_LOG_ERROR, "E", tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) NDF_LOG_(NDF_LOG_WARN,  "W", tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) NDF_LOG_(NDF_LOG_INFO,  "I", tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) NDF_LOG_(NDF_LOG_DEBUG, "D", tag, fmt, ##__VA_ARGS__)

NORETURN void ndf_assert_fail(const char *file, int line, const char *cond);
#if NDF_ASSERT_ENABLE
#define NDF_ASSERT(c) do { if (!(c)) ndf_assert_fail(__FILE__, __LINE__, #c); } while (0)
#else
#define NDF_ASSERT(c) do { (void)sizeof(c); } while (0)
#endif
