/*
 * ESP32-S3 Debug Output HAL
 * Supports USB Serial/JTAG and UART debug output
 */

#ifndef HAL_DEBUG_H
#define HAL_DEBUG_H

#include <stdint.h>
#include <stdarg.h>

/* Debug output backend */
typedef enum {
    DEBUG_USB_SERIAL = 0,   /* USB Serial/JTAG CDC (default on ESP32-S3) */
    DEBUG_UART0 = 1,        /* UART0 */
    DEBUG_UART1 = 2         /* UART1 */
} debug_backend_t;

/* Initialize debug output 
 * For USB Serial: no additional config needed, just call init
 * For UART: will configure 115200 8N1 by default
 */
void debug_init(debug_backend_t backend);

/* Output a single character */
void debug_putc(char c);

/* Output a null-terminated string */
void debug_puts(const char *str);

/* Output a string with newline */
void debug_println(const char *str);

/* Output formatted string (simple printf-like) */
void debug_printf(const char *fmt, ...);

/* Output hex dump of memory */
void debug_hexdump(const void *data, uint32_t len);

/* Output a 32-bit hex value */
void debug_hex32(uint32_t val);

/* Output a 8-bit hex value */
void debug_hex8(uint8_t val);

/* Check if data is available to read (for interactive debug) */
int debug_available(void);

/* Read a character (blocking) */
char debug_getc(void);

/* Flush output buffer */
void debug_flush(void);

/*
 * Debug macros
 */
#ifndef NDEBUG
    #define DBG_INIT(backend)       debug_init(backend)
    #define DBG_PRINT(str)          debug_puts(str)
    #define DBG_PRINTLN(str)        debug_println(str)
    #define DBG_PRINTF(fmt, ...)    debug_printf(fmt, ##__VA_ARGS__)
    #define DBG_HEX32(val)          debug_hex32(val)
    #define DBG_HEX8(val)           debug_hex8(val)
    #define DBG_HEXDUMP(d, l)       debug_hexdump(d, l)
#else
    #define DBG_INIT(backend)       ((void)0)
    #define DBG_PRINT(str)          ((void)0)
    #define DBG_PRINTLN(str)        ((void)0)
    #define DBG_PRINTF(fmt, ...)    ((void)0)
    #define DBG_HEX32(val)          ((void)0)
    #define DBG_HEX8(val)           ((void)0)
    #define DBG_HEXDUMP(d, l)       ((void)0)
#endif

/* Assert macro */
#ifndef NDEBUG
    #define DBG_ASSERT(cond) do { \
        if (!(cond)) { \
            debug_puts("ASSERT FAIL: " #cond "\r\n"); \
            debug_puts("  File: " __FILE__ "\r\n"); \
            while(1) { __asm__ volatile("nop"); } \
        } \
    } while(0)
#else
    #define DBG_ASSERT(cond) ((void)0)
#endif

#endif /* HAL_DEBUG_H */
