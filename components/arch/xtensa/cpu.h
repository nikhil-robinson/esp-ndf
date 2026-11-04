/* CPU helpers for shared code: ccount, core_id, irq_lock / irq_unlock. */
#pragma once
#include "xtensa.h"
#define COOP_CTX_WORDS 6          /* coop_switch context: a0 a1 a12-a15 */
