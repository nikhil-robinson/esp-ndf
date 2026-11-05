/* Dynamic CPU frequency scaling with locks (ESP-IDF esp_pm CPU_FREQ_MAX locks). The PLL stays on and APB stays
 * 80 MHz, so peripherals, UART baud rates and USB are unaffected; only the CPU clock moves between max and min
 * (240 / 160 / 80). pm_lock while work needs speed, pm_unlock when done: the last unlock drops to min. Counted,
 * safe from both cores and from interrupt handlers. */
#pragma once
#include "ndf.h"

int  pm_config(uint32_t max_mhz, uint32_t min_mhz);   /* S3: 240/160/80, P4: 400/200/100; starts at min if no lock is held */
void pm_lock(void);
void pm_unlock(void);
int  pm_locks(void);
