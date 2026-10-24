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
| `NDF_FLASH_SUSPEND`<br>Erase/program auto-suspend | ndf_config.h | `1` | `1` auto: ESP-IDF chip list + SFDP<br>`0` off<br>`2` force (chip tested) | A cache miss from either core suspends a running erase/program in hardware, reads, and resumes it: flash code keeps running during an erase instead of pausing. auto: the chips ESP-IDF validates (GD25Q64/128, XMC-D, FM) and chips whose SFDP table says so. Not for Boya BY25Q (tested: never resumes). |
| `NDF_FLASH_SUS_HOLD_US`<br>Suspend: tSUS (us) | ndf_config.h | `50` | 20 – 400 | Wait after suspend/resume (flash datasheet tSUS; ESP-IDF default 50). |
| `NDF_FLASH_QE_SET`<br>Set quad-enable bit | ndf_config.h | `y` | y / n | If the chip's QE bit is clear, set it (one non-volatile status-register write) so reads use QIO. Bit location per chip as ESP-IDF (SR1 bit 6, SR2 bit 1, 16-bit status, XMC OTP mode). |

## Console

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_CONSOLE`<br>printf output | ndf_config.h | `NDF_CONSOLE_USJ` | `NDF_CONSOLE_USJ` USB Serial/JTAG<br>`NDF_CONSOLE_UART0` UART0<br>`NDF_CONSOLE_NONE` none (dropped) | Where printf() goes. UART0 writes the hardware FIFO directly (safe from ISRs). "none" removes all console traffic. |
| `NDF_CONSOLE_BAUD`<br>UART0 baud | ndf_config.h | `115200` | 1200 – 5000000 | Achieved rate is within 0.1% (fractional divider). |
| `NDF_CONSOLE_TX_PIN`<br>UART0 TX pin | ndf_config.h | `43` | 0 – 48 | 43 = the ROM log pin. |
| `NDF_CONSOLE_RX_PIN`<br>UART0 RX pin | ndf_config.h | `44` | -1 – 48 | -1: no input. |

## Boot

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_BOOT_ROM_LOG_OFF`<br>ROM boot log off after first boot | ndf_config.h | `y` | y / n | Sets the RTC flag the ROM checks (RTC_CNTL_STORE4 bits 0/16, as IDF esp_rom_disable_logging): every reset after the first boot skips the ROM banner. No eFuse is burned; a power-on brings the log back. |
| `NDF_BOOT_BANNER`<br>Print system banner | ndf_config.h | `n` | y / n | Before main(): chip revision, MAC, CPU/APB clocks, flash id/size/mode and read path, and the reset cause of each core. |
| `NDF_BOOT_WDT_OFF`<br>Stop ROM watchdogs at boot | ndf_config.h | `y` | y / n | The ROM leaves the RTC and timer-group watchdogs armed. esp-ndf stops them; a watchdog driver is on the TODO list. |
| `NDF_BOOT_RTC_RESET`<br>Reset RTC pads/touch at boot | ndf_config.h | `y` | y / n | The RTC domain survives resets: pads left in RTC mux and a running touch scanner would leak into the new boot. Pads under hold (rtcio_hold) are kept. |

## Memory

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `XIP`<br>Code from flash (XIP) | app.mk | `lib` | `off` off: everything in internal RAM<br>`lib` libraries in flash<br>`all` libraries, application and storage in flash | Code in flash runs through the 32 KB instruction cache (deterministic only on cache hits). Drivers, interrupt paths, printf, string, heap, DSP kernels and the flash driver always stay in RAM. Mark a function RAM_FN to keep it in RAM, FLASH_FN to put it in flash. Level 4/5 handlers run during flash writes and must be in RAM. |
| `XIP_ADDR`<br>Code blob flash offset | app.mk | `3145728` | 65536 – 15728640 | 64 KB aligned; the xip partition. make flash writes build/<app>/xip.bin there. |
| `XIP_SIZE`<br>Code blob size limit | app.mk | `1048576` | 65536 – 8388608 | Build fails if the blob (code + read-only data in flash) is larger. |
| `NDF_ICACHE_KB`<br>Instruction cache size | ndf_config.h | `32` | `32` 32 KB (SRAM0 fully)<br>`16` 16 KB | IDF default is 16 KB. The image starts at 0x40378000, so all 32 KB of SRAM0 are free for it. |
| `STACK0_SIZE`<br>Core 0 stack (bytes) | app.mk | `16384` | 2048 – 131072 | Stack of main() and core 0 interrupt handlers. Reserved after .bss in internal RAM. |
| `STACK1_SIZE`<br>Core 1 stack (bytes) | app.mk | `16384` | 1024 – 131072 | Stack of the function started with cpu1_start() and core 1 interrupt handlers. |

## Core services

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_SWTIMER_LEVEL`<br>Software timer IRQ level | ndf_config.h | `1` | 1 – 3 | Interrupt level the swtimer callbacks run at. Higher preempts more of the application. |
| `NDF_LOG_LEVEL`<br>Log level | ndf_config.h | `NDF_LOG_INFO` | `NDF_LOG_NONE` none<br>`NDF_LOG_ERROR` error<br>`NDF_LOG_WARN` warning<br>`NDF_LOG_INFO` info<br>`NDF_LOG_DEBUG` debug | LOGx() calls above this level compile to nothing: no code, no strings in the image. |
| `NDF_PANIC_RESTART`<br>Restart after a panic | ndf_config.h | `n` | y / n | After a fault, failed NDF_ASSERT or watchdog report (registers + backtrace) the chip halts so a debugger can attach. On: it restarts instead (field devices). |
| `NDF_ASSERT_ENABLE`<br>NDF_ASSERT checks | ndf_config.h | `y` | y / n | Off: NDF_ASSERT() compiles to nothing. |
| `NDF_UART_RING`<br>UART software ring (bytes) | ndf_config.h | `2048` | 64 – 16384 | Per direction and per port (3 ports), power of 2. Lower it when RAM is tight (an app using the RF dump engine must end below 0x3FCB0000); a small TX ring only makes printf wait. |

## Storage

| Option | File | Default | Values | Description |
|---|---|---|---|---|
| `NDF_VFS_MAX_MOUNTS`<br>VFS: mounted file systems | ndf_config.h | `4` | 1 – 16 | Mount table size. |
| `NDF_VFS_MAX_FILES`<br>VFS: open files + directories | ndf_config.h | `16` | 1 – 64 | Shared descriptor table for all file systems. |
| `NDF_FAT_MAX_FILES`<br>FAT: open files | ndf_config.h | `8` | 1 – 32 | Per-file state is reserved statically. |
| `NDF_FAT_MAX_DIRS`<br>FAT: open directories | ndf_config.h | `4` | 1 – 16 | Open opendir() handles. |
| `NDF_FAT_PATH_CACHE`<br>FAT: path cache entries | ndf_config.h | `4` | 1 – 32 | Recently resolved paths; makes repeated open() ~cheap. |
| `NDF_SPIFFS_MAX_FILES`<br>SPIFFS: open files | ndf_config.h | `4` | 1 – 32 | Open file handles. |
| `NDF_SPIFFS_MAX_DIRS`<br>SPIFFS: open directories | ndf_config.h | `2` | 1 – 16 | Open directory handles. |
| `NDF_SPIFFS_MAX_BLOCKS`<br>SPIFFS: max volume (4 KB blocks) | ndf_config.h | `256` | 16 – 4096 | Largest mountable volume; costs ~30 bytes of RAM per block for the lookup mirror. |
| `NDF_NVS_CACHE`<br>NVS: item cache entries | ndf_config.h | `8` | 1 – 64 | Recently read items; repeated nvs_get hits skip the page scan. |

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
