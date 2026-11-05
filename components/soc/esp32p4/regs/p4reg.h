/* Field helpers for the generated register headers (tools/p4_regs/gen.py): F is a field name without the _R/_S/_W suffix. */
#pragma once
#include "ndf.h"
#define FMASK(F)    (F##_W >= 32 ? 0xFFFFFFFFu : ((1u << F##_W) - 1u) << F##_S)
#define FSET(F)     REG_SET(F##_R, FMASK(F))
#define FCLR(F)     REG_CLR(F##_R, FMASK(F))
#define FPUT(F, v)  REG_FIELD(F##_R, F##_S, F##_W, (v))
#define FGET(F)     ((REG(F##_R) >> F##_S) & (F##_W >= 32 ? 0xFFFFFFFFu : ((1u << F##_W) - 1u)))
#define FV(F, v)    (((uint32_t)(v) << F##_S) & FMASK(F))                 /* the field's bits of value v, for composing a register */
