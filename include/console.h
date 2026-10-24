/* Debug console on USB-Serial-JTAG. Polled, never blocks forever:
 * if no host drains the FIFO, output is dropped after a short timeout. */
#pragma once
#include "ndf.h"
#include <stdarg.h>

void console_init(void);
int  console_write(const char *buf, int n);       /* returns bytes accepted */
int  console_try_write(const char *buf, int n);   /* never waits; for hot loops */
int  console_getc(void);                          /* received byte or -1, never waits */
bool console_host_present(void);                  /* USB host attached (start-of-frame seen in the last 10 ms) */

int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintf(const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  puts(const char *s);
int  putchar(int c);
