"""esp-ndf configuration schema: the single source of truth for menuconfig,
the generated <app>/ndf_config.h + <app>/app.mk, and docs/CONFIG.md.

Each option:
  key      C macro (target 'h') or make variable (target 'mk')
  type     'bool' | 'int' | 'hex' | 'choice' | 'string'
  default  value in the type's form (bool: True/False, choice: one of the values)
  label    one line shown in the menu
  help     longer text for the help pane and the docs
  target   'h' -> <app>/ndf_config.h, 'mk' -> <app>/app.mk
  choices  for 'choice': [(value, label), ...]; value is written as-is
  range    for 'int'/'hex': (min, max)
  depends  optional: key of a bool that must be on, or (key, value) that must match

A menu is ('menu', title, [items]). Read-only chip capabilities come from
soc_caps.h at run time and are shown in their own menu.
"""

SCHEMA = ('menu', 'esp-ndf configuration', [
    ('menu', 'Clock', [
        dict(key='NDF_CPU_MHZ', type='choice', default='240', target='h', label='CPU frequency',
             choices=[('240', '240 MHz (PLL)'), ('160', '160 MHz (PLL)'), ('80', '80 MHz (PLL)'), ('40', '40 MHz (crystal)')],
             help='Set once at boot by clk_cpu_set_mhz(). APB stays at 80 MHz on the PLL settings and equals the CPU '
                  'clock at 40 MHz. All cycle numbers in docs/PERFORMANCE.md are at 240 MHz.'),
    ]),
    ('menu', 'Flash', [
        dict(key='FLASH_MODE', type='choice', default='dio', target='mk', label='Image header: boot read mode',
             choices=[('qio', 'QIO'), ('qout', 'QOUT'), ('dio', 'DIO'), ('dout', 'DOUT')],
             help='Mode the ROM loader uses to copy the image into RAM. At run time the flash driver switches to QIO '
                  'by itself when the chip has its QE bit set, so DIO is the safe default.'),
        dict(key='FLASH_FREQ', type='choice', default='80m', target='mk', label='Image header: boot SPI clock',
             choices=[('80m', '80 MHz'), ('40m', '40 MHz'), ('26m', '26 MHz'), ('20m', '20 MHz')],
             help='SPI clock the ROM loader uses. 80 MHz matches the run-time setting.'),
        dict(key='NDF_FLASH_READ', type='choice', default='NDF_FLASH_READ_AUTO', target='h', label='Read path',
             choices=[('NDF_FLASH_READ_AUTO', 'auto: data cache + QIO (fast)'), ('NDF_FLASH_READ_SPI1', 'SPI1 registers only')],
             help='auto: reads go through the data cache (SPI0 in QIO, 64-byte lines, whole flash mapped by the MMU): '
                  '4 KB cold in ~134 us, repeated reads are cache hits. Falls back to SPI1 if the chip is not in quad '
                  'mode. SPI1 only: ~209 us per 4 KB, leaves the cache untouched.'),
        dict(key='NDF_FLASH_SUSPEND', type='choice', default='1', target='h', label='Erase/program auto-suspend',
             choices=[('1', 'auto: ESP-IDF chip list + SFDP'), ('0', 'off'), ('2', 'force (chip tested)')],
             help='A cache miss from either core suspends a running erase/program in hardware, reads, and resumes it: '
                  'flash code keeps running during an erase instead of pausing. auto: the chips ESP-IDF validates '
                  '(GD25Q64/128, XMC-D, FM) and chips whose SFDP table says so. Not for Boya BY25Q (tested: never resumes).'),
        dict(key='NDF_FLASH_SUS_HOLD_US', type='int', default=100, target='h', label='Suspend: tSUS (us)', range=(20, 400),
             depends=('NDF_FLASH_SUSPEND', '!0'), help='Wait after suspend/resume (flash datasheet tSUS; ESP-IDF default 50).'),
        dict(key='NDF_FLASH_QE_SET', type='bool', default=True, target='h', label='Set quad-enable bit',
             help='If the chip\'s QE bit is clear, set it (one non-volatile status-register write) so reads use QIO. '
                  'Bit location per chip as ESP-IDF (SR1 bit 6, SR2 bit 1, 16-bit status, XMC OTP mode).'),
    ]),
    ('menu', 'Brownout and glitch', [
        dict(key='NDF_FLASH_MHZ', type='choice', default='120', target='h', label='Flash clock at run time',
             choices=[('120', '120 MHz (timing tuned at start, falls back to 80)'), ('80', '80 MHz')],
             help='SPI flash clock after boot. 120 MHz is tuned at start; a chip that fails the tuning runs at 80 MHz.'),
        dict(key='NDF_BROWNOUT', type='int', default=7, target='h', label='Brownout level (0 = off)', range=(0, 7),
             help='1 3.30 V, 2 3.19 V, 3 2.98 V, 4 2.84 V, 5 2.67 V, 6 2.56 V, 7 2.44 V (IDF default 7). On detection flash '
                  'and RF are powered down and the hardware resets the chip (reset cause brownout), as IDF without the interrupt '
                  'option; bod_start() for interrupt mode with a callback. Costs ~9 us of warm boot.'),
        dict(key='NDF_GLITCH_RESET', type='bool', default=True, target='h', label='Clock-glitch and super-WDT resets',
             help='As IDF bootloader: a clock glitch or a starved super watchdog resets the chip.'),
    ]),
    ('menu', 'Network (lwIP)', [
        dict(key='NDF_NET_MEM_SIZE', type='int', default=24576, target='h', label='lwIP heap (bytes)', range=(4096, 262144),
             help='PCBs, TCP segments and outgoing pbufs (components/lwip). RAM counts against the 160 KB below the RF dump engine banks.'),
        dict(key='NDF_NET_PBUFS', type='int', default=8, target='h', label='Receive buffers (1536 B each)', range=(4, 64),
             help='Frames waiting for lwIP; a full pool drops frames and counts them (lwip_stats.link.drop).'),
        dict(key='NDF_NET_TCP_SEGS', type='int', default=6, target='h', label='TCP window / send buffer (segments)', range=(2, 32),
             help='Both the receive window and the send buffer are this many segments of NDF_NET_TCP_MSS.'),
        dict(key='NDF_NET_TCP_MSS', type='int', default=536, target='h', label='TCP segment size', range=(256, 1460),
             help='Small on purpose while the Wi-Fi downlink is 1 Mbit/s and has no hardware ACK (docs/WIFI_MAC.md).'),
    ]),
    ('menu', 'OTA update', [
        dict(key='NDF_OTA', type='bool', default=True, target='h', label='Loader picks the image from otadata',
             help='ESP-IDF otadata format: factory image, or ota_0/ota_1 by sequence number. Adds ~0.75 ms to a cold '
                  'boot (warm resets unchanged). Off: always the image at BOOT_LOAD_ADDR (cold boot 6.0 ms).'),
        dict(key='NDF_OTA_ROLLBACK', type='bool', default=True, target='h', label='Rollback', depends=('NDF_OTA', True),
             help='A new image is checked (SHA-256) once, then must call ota_mark_valid() in its first boot; otherwise the '
                  'next reset marks it aborted and boots the previous one (as IDF APP_ROLLBACK_ENABLE).'),
    ]),
    ('menu', 'AES accelerator', [
        dict(key='NDF_AES_DMA_MIN', type='int', default=4, target='h', label='DMA from this many blocks', range=(1, 4096),
             help='Runs of at least this many 16-byte blocks in internal RAM use the engine DMA mode; shorter runs are fed by the CPU.'),
    ]),
    ('menu', 'SHA accelerator', [
        dict(key='NDF_SHA_DMA_MIN', type='int', default=4, target='h', label='DMA from this many blocks', range=(1, 1000),
             help='Runs of at least this many 64/128-byte blocks in internal RAM go to the engine by DMA (up to 63 '
                  'blocks per run); shorter runs and data in flash/PSRAM are written by the CPU.'),
    ]),
    ('menu', 'Random numbers', [
        dict(key='NDF_RNG_RESEED', type='int', default=65536, target='h', label='Generator reseed interval (bytes)',
             range=(1024, 16777216), help='rng_fill/rng_u32 (ChaCha20, arc4random-style fast key erasure) mix 256 new '
                  'hardware bits into the key after this many bytes. rng_hw is always straight from the hardware.'),
    ]),
    ('menu', 'Console', [
        dict(key='NDF_CONSOLE', type='choice', default='NDF_CONSOLE_USJ', target='h', label='printf output',
             choices=[('NDF_CONSOLE_USJ', 'USB Serial/JTAG'), ('NDF_CONSOLE_UART0', 'UART0'), ('NDF_CONSOLE_NONE', 'none (dropped)')],
             help='Where printf() goes. UART0 writes the hardware FIFO directly (safe from ISRs). "none" removes all console traffic.'),
        dict(key='NDF_CONSOLE_BAUD', type='int', default=115200, target='h', label='UART0 baud', range=(1200, 5000000),
             depends=('NDF_CONSOLE', 'NDF_CONSOLE_UART0'), help='Achieved rate is within 0.1% (fractional divider).'),
        dict(key='NDF_CONSOLE_TX_PIN', type='int', default=43, target='h', label='UART0 TX pin', range=(0, 48),
             depends=('NDF_CONSOLE', 'NDF_CONSOLE_UART0'), help='43 = the ROM log pin.'),
        dict(key='NDF_CONSOLE_RX_PIN', type='int', default=44, target='h', label='UART0 RX pin', range=(-1, 48),
             depends=('NDF_CONSOLE', 'NDF_CONSOLE_UART0'), help='-1: no input.'),
    ]),
    ('menu', 'Boot', [
        dict(key='NDF_BOOT_ROM_LOG_OFF', type='bool', default=True, target='h', label='ROM boot log off after first boot',
             help='Sets the RTC flag the ROM checks (RTC_CNTL_STORE4 bits 0/16, as IDF esp_rom_disable_logging): every '
                  'reset after the first boot skips the ROM banner. No eFuse is burned; a power-on brings the log back.'),
        dict(key='NDF_BOOT_BANNER', type='bool', default=False, target='h', label='Print system banner',
             help='Before main(): chip revision, MAC, CPU/APB clocks, flash id/size/mode and read path, and the reset '
                  'cause of each core.'),
        dict(key='NDF_BOOT_WDT_OFF', type='bool', default=True, target='h', label='Stop ROM watchdogs at boot',
             help='The ROM leaves the RTC and timer-group watchdogs armed. esp-ndf stops them; a watchdog driver is on '
                  'the TODO list.'),
        dict(key='NDF_BOOT_RTC_RESET', type='bool', default=True, target='h', label='Reset RTC pads/touch at boot',
             help='The RTC domain survives resets: pads left in RTC mux and a running touch scanner would leak into the '
                  'new boot. Pads under hold (rtcio_hold) are kept.'),
    ]),
    ('menu', 'Memory', [
        dict(key='XIP', type='choice', default='lib', target='mk', label='Code from flash (XIP)',
             choices=[('off', 'off: everything in internal RAM'), ('lib', 'libraries in flash'), ('all', 'libraries, application and storage in flash')],
             help='Code in flash runs through the 32 KB instruction cache (deterministic only on cache hits). Drivers, '
                  'interrupt paths, printf, string, heap, DSP kernels and the flash driver always stay in RAM. Mark a '
                  'function RAM_FN to keep it in RAM, FLASH_FN to put it in flash. Level 4/5 handlers run during flash '
                  'writes and must be in RAM.'),
        dict(key='XIP_ADDR', type='int', default=0x300000, target='mk', label='Code blob flash offset', range=(0x10000, 0xF00000),
             depends=('XIP', '!off'), help='64 KB aligned; the xip partition. make flash writes build/<app>/xip.bin there.'),
        dict(key='XIP_SIZE', type='int', default=0x100000, target='mk', label='Code blob size limit', range=(0x10000, 0x800000),
             depends=('XIP', '!off'), help='Build fails if the blob (code + read-only data in flash) is larger.'),
        dict(key='NDF_ICACHE_KB', type='choice', default='32', target='h', label='Instruction cache size',
             choices=[('32', '32 KB (SRAM0 fully)'), ('16', '16 KB')], depends=('XIP', '!off'),
             help='IDF default is 16 KB. The image starts at 0x40378000, so all 32 KB of SRAM0 are free for it.'),
        dict(key='STACK0_SIZE', type='int', default=16384, target='mk', label='Core 0 stack (bytes)', range=(2048, 131072),
             help='Stack of main() and core 0 interrupt handlers. Reserved after .bss in internal RAM.'),
        dict(key='STACK1_SIZE', type='int', default=16384, target='mk', label='Core 1 stack (bytes)', range=(1024, 131072),
             help='Stack of the function started with cpu1_start() and core 1 interrupt handlers.'),
    ]),
    ('menu', 'Core services', [
        dict(key='NDF_PSRAM_MALLOC_MIN', type='int', default=16384, target='h', label='malloc: PSRAM from this size', range=(0, 1048576),
             help='Blocks at least this big try PSRAM first (IDF SPIRAM_MALLOC_ALWAYSINTERNAL). 0 = never PSRAM.'),
        dict(key='NDF_SWTIMER_LEVEL', type='int', default=1, target='h', label='Software timer IRQ level', range=(1, 3),
             help='Interrupt level the swtimer callbacks run at. Higher preempts more of the application.'),
        dict(key='NDF_COREDUMP', type='bool', default=True, target='h', label='Core dump to flash on panic',
             help='Registers, backtrace and stacks to the coredump partition; decode with tools/coredump.py.'),
        dict(key='NDF_APPTRACE', type='bool', default=False, target='h', label='App trace over JTAG (trace.h)',
             help='Reserves TRAX blocks 28/29 (0x3FCE8000-0x3FCEFFFF): DRAM ends 4 KB earlier. Host: OpenOCD esp apptrace.'),
        dict(key='NDF_USB_GUARD_MS', type='int', default=3000, target='h', label='USB OTG guard watchdog (ms)', range=(0, 60000),
             help='While the PHY belongs to USB OTG, the RTC watchdog resets the chip (PHY back to USB-JTAG) if '
                  'usb_dev_poll stops for this long. 0 = off (only if the app runs the RTC watchdog itself).'),
        dict(key='NDF_LOG_LEVEL', type='choice', default='NDF_LOG_INFO', target='h', label='Log level',
             choices=[('NDF_LOG_NONE', 'none'), ('NDF_LOG_ERROR', 'error'), ('NDF_LOG_WARN', 'warning'), ('NDF_LOG_INFO', 'info'), ('NDF_LOG_DEBUG', 'debug')],
             help='LOGx() calls above this level compile to nothing: no code, no strings in the image.'),
        dict(key='NDF_PANIC_RESTART', type='bool', default=False, target='h', label='Restart after a panic',
             help='After a fault, failed NDF_ASSERT or watchdog report (registers + backtrace) the chip halts so a '
                  'debugger can attach. On: it restarts instead (field devices).'),
        dict(key='NDF_STACK_GUARD', type='int', default=256, target='h', label='Stack guard margin (bytes, 0 = off)', range=(0, 4096),
             help='The CPU debug assistant watches SP from boot on both cores and panics (or restarts) when it comes within '
                  'this many bytes of the bottom of the stack: overflow caught before it corrupts other memory. No run-time cost.'),
        dict(key='NDF_MEMPROT', type='int', default=1, target='h', label='Memory protection (0 off, 1 on, 2 on + locked)', range=(0, 2),
             help='PMS split at the end of code: stores into code and fetches from data are blocked and reported (panic). '
                  '2 locks the configuration until reset (registers only, no eFuse).'),
        dict(key='NDF_ULP_RESERVE', type='int', default=0, target='h', label='ULP program memory (bytes)', range=(0, 3824),
             help='RTC slow memory kept for a ULP-RISC-V program (ulp.h). RTC_DATA_ATTR variables move above it; loading a ULP '
                  'program makes every reset go through the ROM (the warm-boot jump shares 0x50000000).'),
        dict(key='NDF_ASSERT_ENABLE', type='bool', default=True, target='h', label='NDF_ASSERT checks',
             help='Off: NDF_ASSERT() compiles to nothing.'),
        dict(key='NDF_UART_RING', type='int', default=2048, target='h', label='UART software ring (bytes)', range=(64, 16384),
             help='Per direction and per port (3 ports), power of 2. Lower it when RAM is tight (an app using the RF dump '
                  'engine must end below 0x3FCB0000); a small TX ring only makes printf wait.'),
    ]),
    ('menu', 'Storage', [
        dict(key='NDF_VFS_MAX_MOUNTS', type='int', default=4, target='h', label='VFS: mounted file systems', range=(1, 16),
             help='Mount table size.'),
        dict(key='NDF_VFS_MAX_FILES', type='int', default=16, target='h', label='VFS: open files + directories', range=(1, 64),
             help='Shared descriptor table for all file systems.'),
        dict(key='NDF_FAT_MAX_FILES', type='int', default=8, target='h', label='FAT: open files', range=(1, 32),
             help='Per-file state is reserved statically.'),
        dict(key='NDF_FAT_MAX_DIRS', type='int', default=4, target='h', label='FAT: open directories', range=(1, 16),
             help='Open opendir() handles.'),
        dict(key='NDF_FAT_PATH_CACHE', type='int', default=4, target='h', label='FAT: path cache entries', range=(1, 32),
             help='Recently resolved paths; makes repeated open() ~cheap.'),
        dict(key='NDF_SPIFFS_MAX_FILES', type='int', default=4, target='h', label='SPIFFS: open files', range=(1, 32),
             help='Open file handles.'),
        dict(key='NDF_SPIFFS_MAX_DIRS', type='int', default=2, target='h', label='SPIFFS: open directories', range=(1, 16),
             help='Open directory handles.'),
        dict(key='NDF_SPIFFS_MAX_BLOCKS', type='int', default=256, target='h', label='SPIFFS: max volume (4 KB blocks)', range=(16, 4096),
             help='Largest mountable volume; costs ~30 bytes of RAM per block for the lookup mirror.'),
        dict(key='NDF_IRQ_SHARES', type='int', default=48, target='h', label='IRQ: shared-line members', range=(8, 256),
             help='Sources that share a CPU interrupt line once the free lines of a level are used up (16 B each).'),
        dict(key='NDF_PFS_MAX_FILES', type='int', default=4, target='h', label='PFS: open files', range=(1, 32),
             help='Open file handles (264 B each: 256 B write buffer).'),
        dict(key='NDF_PFS_MAX_DIRS', type='int', default=2, target='h', label='PFS: open directories', range=(1, 16),
             help='Open directory handles.'),
        dict(key='NDF_PFS_MAX_NAMES', type='int', default=32, target='h', label='PFS: files on the volume', range=(4, 1024),
             help='File table in RAM, 72 B per file.'),
        dict(key='NDF_PFS_MAX_EXTENTS', type='int', default=512, target='h', label='PFS: extents on the volume', range=(32, 8192),
             help='Extent list in RAM, 12 B each; a contiguous file of N KB needs about N/4 extents.'),
        dict(key='NDF_PFS_MAX_BLOCKS', type='int', default=256, target='h', label='PFS: max volume (4 KB blocks)', range=(16, 4096),
             help='Largest mountable volume; 2 B of RAM per block.'),
        dict(key='NDF_NVS_CACHE', type='int', default=8, target='h', label='NVS: item cache entries', range=(1, 64),
             help='Recently read items; repeated nvs_get hits skip the page scan.'),
    ]),
    ('menu', 'Partitions', [
        dict(key='PART_TABLE_ADDR', type='hex', default=0x80000, target='mk', label='Partition table offset', range=(0x8000, 0xF00000),
             help='Where the partition table lives. The app image (RAM image loaded from flash 0x0) must end below it; '
                  'the build checks this.'),
        dict(key='PARTS', type='string', default='tools/partitions.csv', target='mk', label='Partition table CSV',
             help='CSV used by "make partition" (same format as ESP-IDF, binary identical to gen_esp32part.py).'),
    ]),
    ('menu', 'Build', [
        dict(key='NO_ACCEL', type='bool', default=False, target='mk', label='Portable build (no hardware acceleration)',
             help='Builds every accelerated path (vector unit, S32C1I, flash cache, loop fix) with its portable '
                  'fallback. Used to test the fallbacks; slower.'),
        dict(key='PORT', type='string', default='', target='mk', label='Serial port for flash/monitor',
             help='Empty: the Makefile default (the devkit by its USB serial id).'),
    ]),
])
