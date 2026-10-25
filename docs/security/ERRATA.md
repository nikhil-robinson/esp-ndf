# ESP32-S3 silicon errata vs esp-ndf (all revisions v0.0 - v0.2)

Source: Espressif ESP32-S3 Series SoC Errata, https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/
(checked 2026-10-25). Re-check this table when a new chip revision or errata version appears.

| Erratum | Issue | esp-ndf status |
|---|---|---|
| CACHE-126 | cache hit error during a D-cache write-back | worked around: components/drivers/psram/psram.c dcache_op - interrupts off + D-cache freeze around every write-back (as the errata workaround) |
| RTC-126 | RTC register reads wrong after light-sleep wake when RTC peripherals were powered down | not reachable: light sleep is parked (docs/TODO.md); the sleep code must keep RTC peripherals powered when it is enabled |
| ANALOG-160 | chip damage with BIAS_SLEEP = 0 and PD_CUR = 1 | not reachable: components/soc/esp32s3/sleep.c writes both fields together (0/0 awake, 1/1 asleep) - never 0/1 |
| LCD-239 | LCD unreliable with CLK_EQU_SYSCLK = 1 | worked around: components/drivers/lcd/i80.c clock_set never selects divider 1 |
| USBOTG-4289 | USB-OTG download mode unavailable | not used: flashing goes over USB-Serial/JTAG |
| RMT-176 | idle level may be wrong in continuous TX | worked around: components/drivers/rmt/rmt.c always sets IDLE_OUT_EN (register-controlled idle level) |
| TOUCH-100 | TOUCH_SCAN_DONE_INT raw value undefined | not used: components/drivers/touch/touch.c uses active/inactive interrupts only |
| ADC-183 | SAR ADC2 cannot work under the digital (DMA) controller | worked around: components/drivers/adc/adc_cont.c is ADC1-only (channel > 9 or unit 2 refused) |
