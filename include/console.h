/* Debug console on USB-Serial-JTAG. Polled, never blocks forever:
 * if no host drains the FIFO, output is dropped after a short timeout. */
#pragma once
#include "ndf.h"
#include <stdarg.h>

void console_init(void);
int  console_write(const char *buf, int n);       /* returns bytes accepted */
int  console_try_write(const char *buf, int n);   /* never waits; for hot loops */

int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintf(const char *fmt, va_list ap);
