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
        dict(key='NDF_SWTIMER_LEVEL', type='int', default=1, target='h', label='Software timer IRQ level', range=(1, 3),
             help='Interrupt level the swtimer callbacks run at. Higher preempts more of the application.'),
        dict(key='NDF_LOG_LEVEL', type='choice', default='NDF_LOG_INFO', target='h', label='Log level',
             choices=[('NDF_LOG_NONE', 'none'), ('NDF_LOG_ERROR', 'error'), ('NDF_LOG_WARN', 'warning'), ('NDF_LOG_INFO', 'info'), ('NDF_LOG_DEBUG', 'debug')],
             help='LOGx() calls above this level compile to nothing: no code, no strings in the image.'),
        dict(key='NDF_PANIC_RESTART', type='bool', default=False, target='h', label='Restart after a panic',
             help='After a fault, failed NDF_ASSERT or watchdog report (registers + backtrace) the chip halts so a '
                  'debugger can attach. On: it restarts instead (field devices).'),
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
