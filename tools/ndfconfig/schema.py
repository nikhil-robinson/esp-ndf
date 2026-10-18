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
             choices=[('NDF_CONSOLE_USJ', 'USB Serial/JTAG'), ('NDF_CONSOLE_NONE', 'none (dropped)')],
             help='Where printf() goes. "none" removes all console traffic (printf returns immediately).'),
    ]),
    ('menu', 'Boot', [
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
        dict(key='STACK0_SIZE', type='int', default=16384, target='mk', label='Core 0 stack (bytes)', range=(2048, 131072),
             help='Stack of main() and core 0 interrupt handlers. Reserved after .bss in internal RAM.'),
        dict(key='STACK1_SIZE', type='int', default=16384, target='mk', label='Core 1 stack (bytes)', range=(1024, 131072),
             help='Stack of the function started with cpu1_start() and core 1 interrupt handlers.'),
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
