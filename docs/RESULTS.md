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

WL after 3 dummy wrap-arounds (move count 3): IDF still reads wl.bin = 299
and the 20 KB file (crc32 018a2379). A run cut off mid-rewrite (chip reset
by esptool at rewrite 32) left a mountable volume on both sides holding the
last completed write (wl.bin = 32): FAT-level loss only, no WL corruption.
Test hygiene: use `esptool --after no_reset` so an app never re-runs (and
gets cut off) between steps.

## SPIFFS (IDF default format: 256 B pages, 4 KB blocks, magic + length, `spiffs` partition 1 MB)

| | esp-ndf | ESP-IDF spiffs |
|---|---|---|
| read 30 KB file | 4 ms | 28 ms |
| write 30 KB file | 81 ms | 1541 ms (incl. GC of our deleted pages) |
| 40 x 30 KB rewrites through 1 MB (GC) | 7392 ms | 10921 ms |
| format 1 MB + mount | 183 ms | — |

Lookup tables mirrored in RAM (2 B per page, 7.5 KB for 1 MB), so page
allocation, file and index lookups never rescan flash; GC moves the block with
the most deleted pages and keeps IDF's 2-free-block reserve.

Interop, both directions: IDF lists ours (`data/big.bin` 30003 B, `hello.txt`
24 B) with content verified (in-place overwrite across a page boundary +
append); ours reads IDF's `idf.txt` and `idf_big.bin` (30000 B, pattern ok).

## NVS encryption (XTS-AES-256, IDF scheme; keys in `nvs_keys`, data in `nvs_enc`)

AES accelerator driver (`drivers/crypto/aes.c`, block mode) + XTS; NVS routes
every entry read/write through it (tweak = partition-relative entry address),
so GC re-encrypts moved entries for their new address.

| | esp-ndf | ESP-IDF (mbedtls XTS) |
|---|---|---|
| nvs_get_u32() on encrypted partition | 4 834 – 13 600 cycles | 82 397 cycles |
| u32 update, encrypted | 467 – 584 us | 1 519 us |

Interop, all directions verified on the board:
- host `nvstool.py gen --keyfile` image -> esp-ndf and IDF both read it (blob crc32 a9bb2442)
- esp-ndf writes (string, 6 KB multi-chunk blob, 400 updates through GC) -> IDF reads all
- IDF writes -> esp-ndf reads; `nvstool.py parse --keyfile` decodes a raw dump, 0 errors
- plaintext never appears on flash; a wrong key reads nothing (CRC mismatch), no writes

Not done: IDF's HMAC key-protection scheme needs an HMAC key burnt into eFuse
(irreversible) - the key derivation is in `nvstool.py keygen --hmac-key`, the
device side takes the derived 64-byte key via `nvs_mount_secure()`.

## Host tools

| tool | check |
|---|---|
| `tools/nvstool.py gen` | byte-identical to IDF nvs_partition_gen.py: single-page, multi-page blob, and encrypted images |
| `tools/nvstool.py parse` | decodes plain/encrypted images and raw dumps, verifies all CRCs |
| `tools/nvstool.py keygen` | IDF keys-partition format (eky, tky, crc32) |

IDF sibling app moved to 0x300000 (it outgrew 0x10000-0x7FFFF and was
overwriting the esp-ndf partition table at 0x80000).
| `tools/mfgtool.py` | per-device images byte-identical to IDF mfg_gen.py (sample config, single- and multi-page blob value sets, 3 devices each, `--fileid id`) |
| `tools/fattool.py mkfat [--wl]` | IDF `fatfsparse.py` extracts our raw and WL images identical to the source tree (`diff -r`); board: esp-ndf (WL+FAT) and IDF (`mount_rw_wl`) both mount the host-built WL image, 20 KB file crc32 af841263 = host |
| `tools/fattool.py parse [--wl] -x` | extracts IDF `fatfsgen.py` and `wl_fatfsgen.py` images identical to the source (IDF marks every 8.3-fitting name lowercase, NTRes 0x18, so README.TXT reads back as readme.txt on both FatFs and ours) |

## NVS boot-time reader (`nvs_boot_read`, the "NVS bootloader" role)

Read-only lookup of a list of keys straight from flash: no mount, no RAM
index, no static state, stack only - usable first thing after reset. Same
contract as IDF 5.4's `nvs_bootloader_read` (IDF 5.3 used for the siblings
does not have it, so it is checked against our IDF-verified NVS instead).

| check | result |
|---|---|
| IDF-written `from_idf` (u32) and `idf_str` (string) | read correctly |
| 8 keys incl. u8/i32/u64/str, 600-version counter (newest wins), missing key, missing namespace, wrong type | all as expected (NOT_FOUND / NOT_FOUND / ERR) |
| time, 6-page partition, 8 keys | 618 us |

## RTC GPIO (GPIO0-21 through the RTC domain) + pad hold

| | esp-ndf | ESP-IDF rtc_gpio |
|---|---|---|
| set level | 21 cycles | 301 cycles |
| get level | 98 cycles | 333 cycles |

Loopback 100/100, pull-up/down, open drain, hold (level frozen while the
register changes) all verified on the jumpers (10<->11, 12<->13).

Hold through a full system reset (`RTC_SW_SYS_RST`), logic analyzer on GPIO4
(examples/rtc_hold, 2 MHz sampling): exactly one rising and one falling edge,
high for 513.1 ms across reset + reboot, zero glitches. Boot code now leaves
held pads alone. Found on the analyzer: the RTC output latch keeps its last
value across resets, so enabling the output before writing the level gave a
1 us glitch - write the level first (documented in rtcio.h).

## ADC continuous (DMA), ADC1, 20 kHz, 1000-sample frames, 4-frame ring

| check | result |
|---|---|
| frame interval (ISR timestamps) | 50.0000 ms, jitter 2 CPU cycles |
| 1 kHz LEDC square on the input | 50 edges/frame, 20.000 samples/period (ideal 20) |
| static low / high | raw 0 / 4095 |
| mid level vs one-shot | 1864 vs 1862 raw (1494 vs 1492 mV) |
| 2-channel pattern | 999/999 neighbours alternate, per-channel values correct |
| 12 start/stop cycles | all good (overruns 0, sync errors 0) |

Found on hardware: stopping the trigger and handing ADC1 back to the RTC
controller mid-conversion leaves the SAR stuck - later digital conversions
are zero words until reset. Stop now drains the conversion in flight and
leaves the controller; one-shot reads take ADC1 back themselves (as IDF).

## Interrupt line sharing

Level-1 CPU lines run out once ~13 sources are attached (the test suite hit
it: TWAI's attach failed silently after ADC DMA took the last line). Levels
1-3 now share the least-used line when none is free; lone sources keep their
dedicated line (GPIO pin -> ISR still 191 cycles). gptimer ISR now checks its
status like every other driver.

## NVS fixes found by long runs

- set_prim/set_str erased the old copy by (page, entry); a GC triggered by
  the write had moved it, so stale copies stayed live: lookups decrypted 37
  copies (160k cycles) and the partition filled up. Erase now follows the
  index slot; mount removes duplicates and orphan blob chunks (as IDF).
- encrypted nvs_get_u32: 4 863 cycles (IDF 82 397); plain: 2 338 (was 8 797, IDF ~26 000).
- nvstool parse resolves blobs after all pages and reports orphan chunks.

## FAT: survive a power cut during a FAT-sector rewrite

Seen on the board: a reset between WL's erase and program of a FAT1 sector
left it all 0xFF; FAT12 reads that as "every cluster end-of-chain", so the
volume looked full (FatFs/IDF read only FAT1 too). FAT2 was intact. Mount now
rebuilds an erased FAT copy from the other one (copies are written in order,
so one is always whole). Test: erase FAT1, then FAT2, remount -> repaired,
free space unchanged, 20 KB file identical.
Data/directory sectors keep the same erase+program window as IDF's WL.

examples/storage: NVS boot counter + one log appended through the VFS to
FAT-on-WL and to SPIFFS each boot (verified over several resets).

## Sharing data between the cores (examples/xcore_bench vs bench/idf_xcore)

Round trip = core 0 sends, core 1 answers, core 0 sees the answer (one way is about half). 10 000 runs each, in CPU cycles (240 = 1 µs).

| Method | esp-ndf | ESP-IDF (FreeRTOS) |
|---|---|---|
| plain 32-bit word | min 30, avg 42, max 49 | min 26, avg 27, **max 1991** (tick interrupts) |
| 16-byte struct with sequence counter | min 37, avg 74, max 78 | — (no IDF equivalent) |
| lock-protected word | min 47, avg 67, max 68 (`xc_spin`) | min 219, avg 222, max 2241 (`portMUX`) |
| lock + unlock, uncontended | 19–23 | 405 |
| cross-core interrupt / call | **143, fixed** (`xc_notify`) | 363 178 – 479 979 (`esp_ipc_call_blocking`, 1.5–2 ms) |
| queue between cores | — | min 2897, avg 3587, max 6224 (FreeRTOS queue) |

Correctness, measured on the board:
- spinlock with both cores incrementing one counter 100 000 times each:
  200 000 exactly (49–53 cycles per lock + increment + unlock while contended)
- 16-byte struct rewritten back to back by core 0 for 100 ms while core 1
  reads: with the sequence counter 0 torn of 201 681 reads; without any
  protection **every read was torn** (201 681 of 201 681)
- writer at 1 MHz: 0 torn with the sequence counter (188 237 reads)

Internal SRAM is uncached on the S3, so no cache maintenance is needed; a
`memw` barrier costs 2–3 cycles.

## Cross-core design decision (examples/xcore_bench2, xcore_bench3, bench/idf_xcore)

Loop = 320-cycle fixed body on each core, both cores running; numbers are
cycles added per iteration.

| Method | added per loop iteration | worst iteration |
|---|---|---|
| plain 32-bit word | +0 | +2 |
| sequence counter, 16 B struct | writer +29..48, reader +28..56 | reader +125 (a retry) |
| sequence counter, 64 B struct | writer +65..71, reader +110..113 | reader +250 |
| spinlock, 16 B struct | +31 | +40 |

| Critical section (5-field update) | per op | contended (both cores) | lost updates |
|---|---|---|---|
| spinlock (S32C1I) | 38 | 36 avg, 78 max | none (40 000 / 400 000 ok) |
| Remote Core Locking (core 1 server) | 74 | 76 avg, 91 max | none |
| IDF portMUX | 406 | 209 / op | none |

| Stream core 0 -> core 1 | per item | round trip |
|---|---|---|
| esp-ndf SPSC FIFO, one at a time | push 18, 51 end to end | 74 |
| esp-ndf SPSC FIFO, batches of 16 | 30 end to end | — |
| FreeRTOS queue | 384 end to end | 2891–6344 |

Memory ordering: memw, acquire/release (L32AI/S32RI) and compiler-only
barriers all cost the same and all pass: 0 torn 64 B reads in ~1.2 M paced
reads, lock torture 400 000 / 400 000. The cost is the SRAM accesses, not the
barrier, so the documented memw stays. A writer rewriting a 64 B struct back
to back (every ~70 cycles) starves the reader; at 300+ cycles per write the
reader keeps up 1:1.

SRAM bank contention (both cores loading at once, same / +32 KB / +64 KB
block): none measurable (24 584 vs 24 585 cycles).

Decision: plain words and sequence counters for loop-to-loop state, SPSC
FIFO for streams, spinlock for rare multi-writer state, cross-core
interrupt (143 cycles) to wake the other core. Remote Core Locking is 2x
slower than the spinlock on this chip and is not used.

## Cross-core interrupt by priority level (examples/xcore_notify)

Core 0 writes SYSTEM_CPU_INTR_FROM_CPU_1; core 1's handler answers. Round trip, 10 000 runs:

| Level | min | avg | max |
|---|---|---|---|
| 1 | 139 | 152 | 153 |
| 3 | 119 | 127 | 128 |
| 5 (fast path) | 103 | **104** | 105 |

Clearing the request after the handler instead of before looks faster in a
back-to-back test only because the next request is caught by the still
running dispatcher; it can also lose a notification, so it is not used.
Polling a shared word stays the lowest-latency path (30–49 round trip); the
interrupt is for waking a core that is busy with something else.

## Interrupt cost: naked level-4/5 handlers + per-core vector tables

Each core now has its own vector table (VECBASE), so a naked assembly
handler (`irq_naked()`) is patched into that core's level-4/5 vector as a
direct jump: no register frame, no C, the handler saves only what it uses.

| Empty handler, CCOMPARE timer | entry | exit | time the interrupted loop loses |
|---|---|---|---|
| C handler, level 1 | 77–81 | 62–66 | 160 |
| C handler, level 3 | 64–67 | 58–60 | 138–144 |
| C handler, level 5 | 47–49 | 45–48 | 109–113 |
| **naked, level 5** | **14** | **12** | **36** |
| (ESP-IDF, C handler level 1, earlier measurement) | 219–405 | — | — |

Entry 14 = first handler instruction ~11 cycles after the match (Cortex-M4: 12).
Loop-register torture (assembly LOOP checksum under ~39 000 interrupts per
level): 0 wrong at levels 1, 3 and 5. Tried and rejected: conditional
LBEG/LEND save and fewer rsyncs in the C path (slower: branches cost more
than the 1-cycle special-register moves they skip).

| Cross-core notify round trip | cycles |
|---|---|
| polling a shared word | 30–49 |
| **naked level-5 handler on the receiving core** | **62, fixed** |
| C handler, level 5 | 96–104 |
| ESP-IDF esp_ipc_call_blocking | 363 178–479 979 |

Found on hardware: clearing the cross-core request is a posted write to a
slow bus; returning before it lands re-fires the interrupt (looked like a
14-cycle round trip - it was the handler re-entering continuously). A read
back of the register after the clear (13 cycles) makes it exact: 10 000
notifications -> 10 000 handler runs. Same read-back added to the C path.

## SIMD (PIE vector unit) kernels vs C and vs Espressif ESP-DSP (bench/idf_dsp)

PIE = coprocessor 3, enabled at boot on both cores (off inside interrupt
handlers, like the FPU). Kernels in lib/dsp.S, our own code. 1024 elements,
16-byte aligned, cycles per call:

| Kernel | C -O2 | esp-ndf | ESP-DSP (S3 build) | vs ESP-DSP |
|---|---|---|---|---|
| int16 dot product | 5 139 | **405** | 1 073 | **2.6x faster** |
| int16 saturating add | 9 237 | **535** | 554 | 3.5 % faster |
| float dot product | 5 141 | **1 570** | 1 587 | 1 % faster (single-issue floor) |
| copy 16 KB | 16 427 | **2 067** (1 902 MB/s) | 2 088 (1 883 MB/s) | equal |

All results equal to the C reference (float: within rounding).

## memcpy / memset (lib/string.c): vector path when safe

128-bit path only for internal DRAM and only when the vector unit is on
(it is off in interrupt handlers - checked with CPENABLE, 1 cycle).

| 8 KB | before | now |
|---|---|---|
| memcpy, both 16-byte aligned | 239 MB/s | **1 738 MB/s** (IDF ROM memcpy: 382) |
| memcpy, same misalignment | 239 MB/s | 1 497 MB/s |
| memcpy, different alignment (unaligned loads + shift-merge) | 60 MB/s | 829 MB/s |
| memset | ~ | **3 249 MB/s** |

Exhaustive check: sizes 0-300 x every source/destination offset 0-17, 0 wrong;
memcpy/memset inside an interrupt handler (scalar path) ok.

## ESP32-S3 core quirk found: re-entered zero-overhead loops run slow

Re-entering the same hardware loop while LBEG still points at it runs at
reduced speed: vector copy 2 055 vs 1 049 cycles (2x), a compiled C loop
10 515 vs 10 016 (+5 %). Changing LBEG before the LOOP instruction removes it
completely (1 053 / 10 017 on every call). Applied everywhere:
- lib/dsp.S: `wsr <count>, lbeg` before each loop
- every C file: the build compiles to assembly and inserts the same
  instruction before each GCC-generated loop (Makefile LOOPFIX), one
  instruction per loop entry, nothing per iteration
Full peripheral suite: all pass after the change.

## Flash read: QIO + overlapped buffer halves (examples/flash_speed)

SPI1 data registers cost 18–24 CPU cycles per read, so draining 64 bytes
left the bus idle ~1.2 µs per transaction. Now: QIO fast read (0xEB, address
on 4 lines, 6 dummy clocks) in 32-byte transactions alternating between the
low (W0-7) and high (W8-15) buffer halves; the CPU drains one half while the
next transfer runs.

| 4 KB read | before | now |
|---|---|---|
| time | 269 µs | **209 µs** (IDF esp_flash: 462 µs) |
| data check | — | sizes 1-600 x 4 offsets 0 wrong; crc32 of 4 KB = host file (ecfd9371) |

Knock-on (full suite, all pass): nvs_get_u32 2 338 -> 1 747 cycles, encrypted
4 863 -> 4 711, boot reader 618 -> 456 µs, FAT open 65 882 -> 51 489 cycles.

## SoC capability flags + portable fallbacks

`soc/esp32s3/soc_caps.h` (C and assembly) and `soc/esp32s3/soc.mk` (make)
describe the chip: cores, cross-core interrupt, FPU, PIE vector unit,
S32C1I, hardware-loop LBEG quirk, and the CPENABLE value at boot. Gated:
lib/dsp.S (PIE) with lib/dsp.c as the portable twin, the vector memcpy/memset
paths, xc_spin (S32C1I, else Peterson), xc_notify (cross-core interrupt), the
LOOPFIX build step and the coprocessor enable at boot.

`make NO_ACCEL=1` builds with every accelerated path off (into
build/<app>-noaccel): periph_test all pass; dsp_bench results equal; mem_bench
exhaustive 0 wrong + ISR ok; xcore_bench2 Peterson lock 40 000 / 40 000.
The portable memcpy was also rewritten (align + 16-byte word unroll, and
shift-merge for different alignments): 8 KB same misalignment 65 564 -> 5 250
cycles, different alignment 65 564 -> 20 606.

## SPI master: register-path overhead (examples/spi_probe, analyzer on CS/SCLK)

Analyzer: 10 MHz -> CS low 3.3 µs with 32 clocks; 40 MHz -> CS low 0.83 µs;
the clocks are right. Software overhead per transfer was ~530 cycles of
peripheral register traffic (~20 cycles per access, read-modify-writes).
Now absolute writes (USER cached per host, CMD written, DMA_CONF untouched
in PIO) and word packing for aligned buffers.

| | before | now | IDF |
|---|---|---|---|
| 4-byte transfer @40 MHz (wire 192) | 730 | **549** | 4 756 (10 MHz) |
| 64 B PIO @40 MHz (wire 3 072) | 5 844 | **3 939** | 8 407 |

Bug found while measuring: the DONE-flag clear is a posted write, so a back-
to-back transfer could see the previous DONE and return before its own last
bit (the old periph_test metric "725 cycles at 10 MHz" was shorter than the
768-cycle wire time - that was the symptom). The start now waits for DONE to
read back cleared: 32-byte back-to-back min 6 805 cycles at 10 MHz (wire
6 144), 400 varied echo transfers 0 bytes wrong, master<->slave modes 0-3 ok.
