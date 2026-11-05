/* ULP-RISC-V coprocessor (S3: TRM 2; IDF ulp_riscv.c sequence). On the P4 the LP core plays this part (soc/esp32p4/ulp.c:
 * program linked at LP RAM, reset handler at +0x80; the LP timer alarm is one-shot, the program re-arms it, tick count in the
 * two words at LP RAM + NDF_ULP_RESERVE; no ULP-FSM, ulp_fsm_run returns -1). The program is a raw binary linked at 0 (RTC slow memory
 * 0x50000000 for the main CPU; RTC registers at 0x8000 for the ULP), at most NDF_ULP_RESERVE bytes. ulp_load switches
 * the CPU reset vector to the ROM first: the warm-boot jump lives at 0x50000000 too. ulp_run(period): the ULP timer
 * restarts the program every period_us (the program halts itself: DONE | SHUT_RESET_EN, as IDF's ulp_riscv_halt). */
#pragma once
#include "ndf.h"

int  ulp_load(const void *bin, size_t len);
int  ulp_run(uint32_t period_us);
void ulp_stop(void);
int  ulp_fsm_run(uint32_t entry_word, uint32_t period_us);   /* ULP-FSM: same ulp_load / ulp_stop; entry = word index */
#ifndef SOC_ULP_MEM_BASE
#define SOC_ULP_MEM_BASE 0x50000000u            /* S3: RTC slow memory; P4: LP RAM (soc_caps.h) */
#endif
static inline volatile uint32_t *ulp_mem(uint32_t off) { return (volatile uint32_t *)(SOC_ULP_MEM_BASE + off); }   /* shared words */
