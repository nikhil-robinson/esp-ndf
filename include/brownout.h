/* Brownout detector (supply below a threshold) and clock-glitch detector (TRM 10; ESP-IDF esp_system/port/brownout.c,
 * bootloader_soc.c). Levels: 1 3.30 V, 2 3.19 V, 3 2.98 V, 4 2.84 V, 5 2.67 V, 6 2.56 V, 7 2.44 V (IDF Kconfig).
 * On a brownout flash and RF are powered down at once, fn runs (if given), and the chip restarts: sys_reset_cause()
 * reads RESET_BROWNOUT. At boot (NDF_BROWNOUT): bod_reset_mode, the hardware resets the chip, no handler code. */
#pragma once
#include "ndf.h"

int  bod_start(int level, void (*fn)(void));
void bod_stop(void);
void bod_reset_mode(int level);             /* hardware resets the chip on a brownout (no handler) */
bool bod_low(void);                         /* supply below the threshold right now */
void glitch_reset(bool on);                 /* reset the chip on a clock glitch (IDF bootloader default: on) */
