/* P4 LP mailbox (lp_mbox.h): ESP-IDF v6.1 hal lp_mailbox_ll.h. The 16 words are at LP_MBOX_BASE + 4 i (always-on domain), the HP-side
 * interrupt flags at +0x50 (raw) / +0x5c (clear), the clock gate at +0x60. Registers: regs/lp_mailbox.h. NOT VERIFIED ON HARDWARE. */
#include "lp_mbox.h"
#include "soc.h"
#include "regs/lp_mailbox.h"

void lp_mbox_init(void) { REG(MB_REG_CLK_EN_REG) = 1; }
void lp_mbox_send(int i, uint32_t v) { if ((unsigned)i < LP_MBOX_MESSAGES) REG(MB_MASSEGE_0_REG + 4 * i) = v; }
uint32_t lp_mbox_read(int i) { return (unsigned)i < LP_MBOX_MESSAGES ? REG(MB_MASSEGE_0_REG + 4 * i) : 0; }
uint32_t lp_mbox_pending(void) { return REG(MB_HP_INT_RAW_REG) & 0xFFFF; }
void lp_mbox_clear(uint32_t mask) { REG(MB_HP_INT_CLR_REG) = mask & 0xFFFF; }
