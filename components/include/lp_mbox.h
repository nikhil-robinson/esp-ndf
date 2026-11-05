/* LP mailbox (ESP32-P4 only, TRM chapter 24): 16 message words shared by the HP cores and the LP core, with one interrupt flag per
 * message for each side. A write by the HP side raises the LP-side flag of that message, a write by the LP side raises the
 * HP-side flag (hardware, on the write of the message register). Use with the ULP program (ulp.h). */
#pragma once
#include "ndf.h"

#define LP_MBOX_MESSAGES 16
void     lp_mbox_init(void);
void     lp_mbox_send(int i, uint32_t v);          /* HP -> LP: message i, flags the LP side */
uint32_t lp_mbox_read(int i);
uint32_t lp_mbox_pending(void);                    /* bit i: the LP side wrote message i since it was cleared */
void     lp_mbox_clear(uint32_t mask);
