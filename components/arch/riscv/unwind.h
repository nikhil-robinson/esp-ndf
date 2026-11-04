/* Panic backtraces on RISC-V (components/arch/riscv/unwind.c). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Same contract as arch/xtensa/unwind.h: emit(pc) per frame, false = no exact unwind (caller scans the stack).
 * a0 = ra of the faulting context, a15 unused. */
bool ndf_unwind(uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a15, void (*emit)(uint32_t pc));
