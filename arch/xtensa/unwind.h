/* Exact stack unwinding for panic reports (arch/xtensa/unwind.c). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Walks the stack from (pc, sp, a0, a15), calling emit(pc) for each frame,
 * the first one included. Returns false when the image carries no unwind
 * table (then the caller falls back to a stack scan). */
bool ndf_unwind(uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a15, void (*emit)(uint32_t pc));
