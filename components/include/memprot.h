/* Memory protection (PMS, TRM 15): internal SRAM split at the end of code. Below: execute + read only (no store
 * through either bus). Above: read + write, no execute. A violation is blocked and reported by the per-core monitor
 * interrupt: panic with the address, or fn. NDF_MEMPROT arms it at boot (1; 2 also locks it until reset). */
#pragma once
#include "ndf.h"

enum { MEMPROT_IRAM = 0, MEMPROT_DRAM = 1 };                         /* which bus saw it */
typedef void (*memprot_fn_t)(int core, int bus, uint32_t addr, bool write);
int  memprot_init(bool lock);                     /* split at _text_end; 0, or -1 if already locked differently */
void memprot_on_violation(memprot_fn_t fn);       /* NULL = panic (default) */
bool memprot_code_write(bool open);               /* open/close stores into code (vector patching); false if locked */
