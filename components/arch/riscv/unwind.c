/* RISC-V has no unwind table in this image yet (mkunwind.py is Xtensa-only): report the faulting pc and ra exactly and
 * let the caller fall back to its stack scan for the rest.
 * ponytail: stack scan only; build an unwind table from .eh_frame (-fasynchronous-unwind-tables) when exact
 * backtraces matter on the P4. */
#include "unwind.h"

bool ndf_unwind(uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a15, void (*emit)(uint32_t pc))
{
    (void)pc; (void)sp; (void)a0; (void)a15; (void)emit;
    return false;
}
