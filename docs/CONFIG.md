# esp-ndf configuration

Generated from `tools/ndfconfig/schema.py` by `tools/ndfconfig/ndfconfig.py --doc`. Change options with `make menuconfig APP=<app>`. That writes only the non-default values to `<app>/ndf_config.h` (C options) and `<app>/app.mk` (build options). Chip capabilities (what the silicon has) are in `soc/<chip>/soc_caps.h`.


## Clock

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_CPU_MHZ`<br>CPU frequency | ndf_config.h | `240` | `240` 240 MHz (PLL)<br>`160` 160 MHz (PLL)<br>`80` 80 MHz (PLL)<br>`40` 40 MHz (crystal) | Set once at boot by clk_cpu_set_mhz(). APB stays at 80 MHz on the PLL settings and equals the CPU clock at 40 MHz. All cycle numbers in docs/PERFORMANCE.md are at 240 MHz. |

## Flash

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `FLASH_MODE`<br>Image header: boot read mode | app.mk | `dio` | `qio` QIO<br>`qout` QOUT<br>`dio` DIO<br>`dout` DOUT | Mode the ROM loader uses to copy the image into RAM. At run time the flash driver switches to QIO by itself when the chip has its QE bit set, so DIO is the safe default. |
| `FLASH_FREQ`<br>Image header: boot SPI clock | app.mk | `80m` | `80m` 80 MHz<br>`40m` 40 MHz<br>`26m` 26 MHz<br>`20m` 20 MHz | SPI clock the ROM loader uses. 80 MHz matches the run-time setting. |
| `NDF_FLASH_READ`<br>Read path | ndf_config.h | `NDF_FLASH_READ_AUTO` | `NDF_FLASH_READ_AUTO` auto: data cache + QIO (fast)<br>`NDF_FLASH_READ_SPI1` SPI1 registers only | auto: reads go through the data cache (SPI0 in QIO, 64-byte lines, whole flash mapped by the MMU): 4 KB cold in ~134 us, repeated reads are cache hits. Falls back to SPI1 if the chip is not in quad mode. SPI1 only: ~209 us per 4 KB, leaves the cache untouched. |

## Console

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_CONSOLE`<br>printf output | ndf_config.h | `NDF_CONSOLE_USJ` | `NDF_CONSOLE_USJ` USB Serial/JTAG<br>`NDF_CONSOLE_NONE` none (dropped) | Where printf() goes. "none" removes all console traffic (printf returns immediately). |

## Boot

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_BOOT_BANNER`<br>Print system banner | ndf_config.h | `n` | y / n | Before main(): chip revision, MAC, CPU/APB clocks, flash id/size/mode and read path, and the reset cause of each core. |
| `NDF_BOOT_WDT_OFF`<br>Stop ROM watchdogs at boot | ndf_config.h | `y` | y / n | The ROM leaves the RTC and timer-group watchdogs armed. esp-ndf stops them; a watchdog driver is on the TODO list. |
| `NDF_BOOT_RTC_RESET`<br>Reset RTC pads/touch at boot | ndf_config.h | `y` | y / n | The RTC domain survives resets: pads left in RTC mux and a running touch scanner would leak into the new boot. Pads under hold (rtcio_hold) are kept. |

## Memory

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `STACK0_SIZE`<br>Core 0 stack (bytes) | app.mk | `16384` | 2048 – 131072 | Stack of main() and core 0 interrupt handlers. Reserved after .bss in internal RAM. |
| `STACK1_SIZE`<br>Core 1 stack (bytes) | app.mk | `16384` | 1024 – 131072 | Stack of the function started with cpu1_start() and core 1 interrupt handlers. |

## Partitions

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `PART_TABLE_ADDR`<br>Partition table offset | app.mk | `0x80000` | 0x8000 – 0xF00000 | Where the partition table lives. The app image (RAM image loaded from flash 0x0) must end below it; the build checks this. |
| `PARTS`<br>Partition table CSV | app.mk | `tools/partitions.csv` | text | CSV used by "make partition" (same format as ESP-IDF, binary identical to gen_esp32part.py). |

## Build

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NO_ACCEL`<br>Portable build (no hardware acceleration) | app.mk | `n` | y / n | Builds every accelerated path (vector unit, S32C1I, flash cache, loop fix) with its portable fallback. Used to test the fallbacks; slower. |
| `PORT`<br>Serial port for flash/monitor | app.mk | `""` | text | Empty: the Makefile default (the devkit by its USB serial id). |

## Chip capabilities (read-only, soc_caps.h)

| Capability | ESP32-S3 | Meaning |
|---|---|---|
| `SOC_CPU_CORES` | 2 |  |
| `SOC_HAS_CROSS_CORE_INT` | 1 | SYSTEM_CPU_INTR_FROM_CPU_n |
| `SOC_CPU_HAS_FPU` | 1 | single-precision FPU, coprocessor 0 |
| `SOC_CPU_HAS_PIE` | 1 | 128-bit vector unit (EE.*), coprocessor 3 |
| `SOC_CPU_HAS_S32C1I` | 1 | compare-and-swap |
| `SOC_CPU_HWLOOP_LBEG_QUIRK` | 1 | re-entered LOOP runs slow unless LBEG changes |
| `SOC_HAS_FLASH_CACHE` | 1 | SPI0 + MMU + data cache: flash reads via 0x3C000000 |
