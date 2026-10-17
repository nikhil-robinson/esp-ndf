# esp-ndf vs ESP-IDF 5.3.5 — measured on hardware

Board: ESP32-S3 devkit (MAC 24:58:7C:E2:C9:D0), CPU 240 MHz on both stacks.
esp-ndf: `make APP=examples/periph_test flash`. ESP-IDF: `bench/idf_periph` (official
driver APIs, IRAM callbacks, `-O2`). Cycles are CPU cycles (4.17 ns). Lower is better.

## Interrupts (`examples/irq_latency` vs `bench/idf_irq`)

| Path, timer match → first C line | esp-ndf | ESP-IDF |
|---|---|---|
| Level 1 | **76 cycles, 0 jitter** | 219–405 |
| Level 3 | **63 cycles, 0 jitter** | 214–274 |
| Level 5 (fast) | **46 cycles, 0 jitter** | asm only |

## GPIO

| | esp-ndf | ESP-IDF |
|---|---|---|
| set + clear | **10 cycles** | 95 |
| pin edge → ISR | **191–192 cycles** | 356–393 |
| dedicated GPIO set + clear | **3 cycles** | 45 |

## GPTimer

| | esp-ndf | ESP-IDF |
|---|---|---|
| read counter | **110 cycles** | 407 |
| alarm → callback | **800 ns, fixed** | 2825–6750 ns |
| 10 kHz period jitter (L1) | **12 cycles** | 828 cycles |
| 10 kHz period jitter (L5 fast path) | **18 cycles** | n/a |
| rate check (1 MHz over 10 ms) | exactly 10000 | — |

## LEDC

| | esp-ndf | ESP-IDF |
|---|---|---|
| duty update | **35 cycles** | 763 |
| reported frequency, 33333 Hz / 8 bit | **33333.333 Hz** (exact, from divider) | 33333 (integer) |

## MCPWM

| | esp-ndf | ESP-IDF |
|---|---|---|
| compare update | **3 cycles** | 75 |
| period interrupt jitter, 20 kHz | **0 cycles** | 21 |
| duty, edge & center aligned (captured) | exact to the 12.5 ns tick | same hardware |
| dead band 1 µs (captured) | 81/81 ticks (±1 capture tick) | same hardware |
| 3-phase sync, 90° | 1001/1000 ticks | same hardware |

## PCNT (x4 quadrature from MCPWM, pads looped back)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 1 MHz, 100 ms, reading continuously (expect 400000) | 400004, 0 backward steps | 400024, 0 backward steps |
| 100 kHz, 1 s, wraps counted by IRQ only | exactly 400000 | — |
| 20 kHz fwd / rev, 10 ms | 800 / −800 | — |
| read count | **85 cycles** | 331 |

Independently checked with the logic analyzer: A and B at 1.000 MHz, B lags 90°.

## RMT (TX GPIO10 → jumper → RX GPIO11, 100 ns ticks)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 100-item frame (> 48: TX refill + RX drain) | 100 received, 0 tick error | 100 received, 0 tick error |
| start transmit (100 items) | **1156 cycles** | 7775 |

## SDM (sigma-delta, 10 MHz, pad sampled 200k times)

| density | esp-ndf | ESP-IDF |
|---|---|---|
| −64 / 0 / +64 (expect 25 / 50 / 75 %) | 25.0 / 50.0 / 75.0 % | 25.0 / 50.0 / 74.9 % |
| density update | **35 cycles** | 318 |

## UART (UART1 TX GPIO12 → jumper → RX GPIO13, 4096 random bytes)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 115200: bytes ok / time (wire 355.6 ms) | 4096, 355.6 ms | 4096, 356.5 ms |
| 2 Mbaud (wire 20.48 ms) | 4096, 20.50 ms | 4096, 20.58 ms |
| 5 Mbaud (wire 8.19 ms) | 4096, 8.21 ms | 4096, 8.27 ms |
| write 16 bytes | **279 cycles** | 2464 |

Baud checked with the logic analyzer (UART2 0x55 on GPIO4): 115197 baud measured for 115200.

## I2C (master I2C0 SDA10/SCL12 ↔ jumpers ↔ slave I2C1 SDA11/SCL13, 100 kHz)

| | esp-ndf | ESP-IDF |
|---|---|---|
| write 16 bytes to slave | 0 wrong, **1551 µs** (≈ wire time) | 0 wrong, 1785 µs |
| write 100 bytes (FIFO chunking) | 0 wrong | 0 wrong |
| read 8 bytes from slave | 0 wrong | — |
| absent address | NACK reported | error reported |
| SCL frequency (analyzer, 400 kHz request) | 399.0 kHz | — |

400 kHz is skipped on this wiring: measured bus rise time with internal
pull-ups is SDA 508 ns / SCL 429 ns, above the 300 ns Fast-mode limit for any
driver. Add 2.2–4.7 kΩ pull-ups to test 400 kHz.

## GDMA (memory to memory, 32 KB)

| | esp-ndf | ESP-IDF (`esp_async_memcpy`) |
|---|---|---|
| 32 KB copy | **63 MB/s**, 0 wrong | 43 MB/s, 0 wrong |

M2M must borrow a valid peripheral id (0–9); an invalid one is refused by the
PMS with a descriptor error (found on hardware).

## SPI (SPI2 master, MOSI GPIO10 → jumper → MISO GPIO11, SCLK GPIO6 on analyzer)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 64 B, 10 / 20 / 40 MHz | 0 wrong, 14649 / 8475 / 5844 cycles | 0 wrong, 17634 / 11469 / 8407 cycles |
| 4 KB DMA @10 MHz (wire 3276 µs) | 0 wrong, 3281 µs | 3338 µs, **RX open item** ¹ |
| 4-byte transaction | **724 cycles** | 4756 |
| SCLK by analyzer (10 MHz) | 10.000 MHz | — |
| master ↔ slave (SPI3), 32 B, modes 0–3 | 0 wrong both directions | — |

¹ In this loopback IDF's 4 KB DMA transfer receives only the first 64 bytes
(unresolved: IDF pin setup for an internal pad loopback; IDF's 64-byte path is correct).

## I2S (I2S0 TX master → jumpers → I2S1 RX slave, 48 kHz 16-bit stereo)

| | esp-ndf | ESP-IDF |
|---|---|---|
| streaming continuity (counter, ~32k samples) | 0 discontinuities, 0 under/overruns | 0 discontinuities |
| BCK measured by analyzer (target 1536 kHz) | **1535.949 kHz** | 1535.947 kHz |
| WS measured by analyzer (target 48 kHz) | 47.998 kHz | — |
| write 1 KB into the ring | **1202 cycles** | (blocking API, not comparable) |

Found with the analyzer: the fractional MCLK divider only engages when its
fields are written one at a time in order (whole-word write ran at 48.077 kHz).
RXEOF_NUM is in bytes.

## ADC one-shot (ADC1 ch9 = GPIO10, voltage set through the GPIO11 jumper, 12 dB)

| | esp-ndf | ESP-IDF (adc_oneshot + curve fitting) |
|---|---|---|
| 0 V | raw 0 → 0 mV | raw 0 → 0 mV |
| pull-up/pull-down divider | raw 1859–1870 → 1490–1499 mV | raw 1859 → 1490 mV |
| 3.3 V (above range) | raw 4095 → 3063 mV | raw 4095 → 3063 mV |
| calibration math, same raw code | **bit-identical** (1859→1490, 4095→3063) | reference |
| one read | **1846 cycles** | 6736 |

Calibration uses this chip's eFuse (BLK_VERSION_MAJOR 1): init code into the
analog front end over regi2c, then IDF's curve-fitting polynomial in integer math.
The eFuse reader also returns the MAC (checked: 24:58:7C:E2:C9:D0).

## TWAI / CAN (TX GPIO10 → jumper → RX GPIO11, self-test / no-ACK)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 100 frames std+ext, DLC 0–8 @125k / 500k / 1M | 100 / 100 / 100 identical | 100 / 100 / 100 identical |
| acceptance filter (0x123 in, 0x124 out) | correct | — |
| send 8 bytes @1 Mbit/s incl. wire + self-rx | **30330 cycles** | 33108 |
| bit time by analyzer @500k | 2.000 µs (48 samples @24 MHz) | — |

## Touch sensor (raw counts, FSM timer scan, IDF defaults)

| pad | esp-ndf | ESP-IDF |
|---|---|---|
| 1 (bare) | 14603 | 14477 |
| 14 (bare) | 19843 | 20150 |
| 10 (jumper wire + GPIO11 pad) | 49053 | 44495 ¹ |
| read | **100 cycles** | 529 |

¹ IDF's `gpio_reset_pin(11)` leaves a pull-up on the far end of the jumper; ours leaves it floating.

## SPI flash (sector 0xF00000, 4 KB)

| | esp-ndf | ESP-IDF (esp_flash) |
|---|---|---|
| erase 4 KB (already blank) | 642 µs | 1041 µs |
| write 4 KB | **10.8 ms** | 12.4 ms |
| read 4 KB | **269 µs** (quad output, QE checked) | 462 µs |
| data check | 0 wrong | 0 wrong |

Incident while developing: an address-encoding bug (left-aligned instead of
plain) erased and rewrote flash sector 0 (our image) once; recovered by
reflashing. The driver now refuses addresses beyond 16 MB.

## Partition table

| | esp-ndf | ESP-IDF |
|---|---|---|
| CSV → binary generator (`tools/mkpart.py`) | **byte-identical** to `gen_esp32part.py` (3072 bytes, MD5 entry) | reference |
| parse + MD5 verify on device | 4 entries, MD5 ok | — |

esp-ndf layout: RAM image in 0x0–0x7FFFF (build fails above 512 KB), table at
0x80000. (The test image had grown past 0x8000 and overwrote the IDF-style table.)

## NVS (IDF on-flash format v2, partition at 0x90000, 6 pages)

| | esp-ndf | ESP-IDF |
|---|---|---|
| esp-ndf writes → IDF reads (u8, i32, u64, str, 3 KB blob, 10 KB multi-chunk blob, counter) | — | **7/7 correct** |
| IDF writes → esp-ndf reads (u32, str, 5000 B blob) | **all correct** | — |
| CRC convention, "123456789" | d202d277 | d202d277 |
| 600 updates on a 6-page partition (GC) | 0 failures, **526 µs** each | 691 µs each |
| get u32 | **8641 cycles** | 26269 |

## LCD (ESP32-S3-BOX-3, ILI9342C over SPI @40 MHz, 320x240 RGB565)

| | esp-ndf |
|---|---|
| full-screen fill (wire time 30.72 ms) | **30.82 ms** |
| color bars incl. CPU pixel generation | 38.56 ms |
| visual check | **confirmed by user**: bars, fill, gradient, animation |

## ESP32-S3-BOX-3 integration (real devices)

| | result |
|---|---|
| I2C scan @100 k and @400 k (board pull-ups) | 0x18 ES8311, 0x40 ES7210, 0x68 IMU at both speeds |
| IMU WHO_AM_I (reg 0x75) | 0x60 (ICM-42607-P) |
| ES8311 chip ID (0xFD/0xFE) | 0x83 0x11 |
| I2S → ES8311 → speaker, 48 kHz (MCLK 256 fs) | **confirmed audible by user** (1 kHz tone + chime) |

## FAT filesystem (own FAT12/16/32 + long names, raw `fat` partition, 4 KB sectors)

| direction | result |
|---|---|
| esp-ndf writes → ESP-IDF FatFs mounts (raw RO, LFN on) and reads | same tree incl. long names; 20 KB file crc32 **018a2379 = 018a2379** |
| IDF `fatfsgen.py` (PC) builds image → esp-ndf reads | nested LFN dirs, JSON, 30 KB file crc32 **ae065942 = ae065942** (host) |
| format 1 MB FAT12 | 335 ms |
| 20 KB write (raw: erase+program per 4 KB) / read | 381 ms / 2 ms |

ESP-IDF's default config has LFN off (it then sees only `DATA~1`-style names).
Memory: the linker now packs data directly after the code's DRAM alias (no
fixed IRAM/DRAM split), and refuses images whose RAM use reaches the ROM area.

## Wear levelling (IDF WL v2 on-flash format, `fat` partition, 4 KB sectors)

Layout and translation follow IDF's `WL_Flash` (2 state copies + config at
the partition end, 1 rotating dummy sector, move every 16 erases). FAT now
mounts on WL instead of the raw partition.

| check | result |
|---|---|
| geometry | 250 usable sectors of 256 (same as IDF) |
| 300 file rewrites | dummy moved 94 sectors, data intact, remount ok |
| dummy wrap (pos 191 -> 34, state rewrite) | data intact after remount |
| IDF `esp_vfs_fat_spiflash_mount_rw_wl` reads ours | tree OK, 20 KB file crc32 018a2379, wl.bin = 299 |
| ours reads IDF's after its 200 rewrites (dummy 188) | idf.txt = "written by IDF WL, pass 199" |
| ours adopts IDF's state | same device id af0f1e88, no reformat |

Gotcha found: the state CRC covers 60 bytes (8 header words + 7 reserved);
with a wrong length IDF silently re-initialises the WL state and the volume
is lost.

## VFS (prefix mount table, FAT registered at /fat)

| | esp-ndf | ESP-IDF (newlib VFS + FatFs + WL) |
|---|---|---|
| open "/fat/hello.txt" (cold, path lookup on flash) | 65 882 cycles | 70 187 cycles |

Longest-prefix routing ("/fatx" does not match "/fat"), fd 0..2 = console,
unregister refused while files are open.
