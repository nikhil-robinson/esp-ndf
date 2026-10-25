/* App trace over JTAG (ESP-IDF apptrace protocol, OpenOCD "esp apptrace start file://out.bin ..."): data goes to the
 * host through the two 16 KB TRAX memory blocks while the program runs - no UART, no halting. Build with
 * NDF_APPTRACE 1 (app ndf_config.h): blocks 28/29 (0x3FCE8000-0x3FCEFFFF) leave DRAM. Target -> host only.
 * Post-mortem when no host is attached (the newest data stay readable after a halt), streaming once OpenOCD connects. */
#pragma once
#include "ndf.h"

int  trace_init(void);                                    /* on each core that writes */
int  trace_write(const void *data, size_t len, uint32_t timeout_us);   /* len <= 16 KB - 4; -1: host busy past timeout */
int  trace_flush(uint32_t timeout_us);                    /* expose what is buffered now */
bool trace_host_connected(void);
