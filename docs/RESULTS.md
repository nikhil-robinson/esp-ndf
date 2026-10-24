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

## I2S TDM and PDM (`periph_test` i2s_modes vs `bench/idf_i2s_modes`, C9:D0, same jumpers)

| | esp-ndf | ESP-IDF |
|---|---|---|
| TDM 4x16 bit, TX slot 2 masked, counter stream | 0 errors, every 4th slot 0 | 0 errors, same |
| TDM TX init | **24.4k cycles** | 181k cycles |
| PDM I2S0 duplex loop (TX PCM->PDM -> jumper -> RX PDM->PCM, 1 kHz) | 1000 Hz, peak +-5363, SNR 33 dB | 998 Hz, peak +-5363, SNR 38 dB |
| PDM init | **27.4k (TX+RX)** | 167k (TX alone) |

Found on the way: (1) masked TDM TX slots take no buffer data; (2) at 4 slots the
ring lost EOF interrupts (two merged into one) - both ring ISRs now catch up to
the descriptor the DMA reports; (3) out EOF means fetched, not played: writer
keeps one block of margin; (4) PDM exists only on I2S0; (5) the fractional MCLK
divider jitters the PDM clock (SNR 5-8 dB) - integer divider as IDF, so fs is
52.083 kHz for 48 kHz asked (same as IDF; init returns the real rate).

## SPI half duplex master + slave HD (`periph_test` spi_hd vs `bench/idf_spi_hd`, C9:D0, no jumpers)

SPI2 master pads 14/15/16-18/21 (+38-41), SPI3 HD slave drives 1/2/42/47, inputs crossed in the GPIO matrix.

| | esp-ndf | ESP-IDF |
|---|---|---|
| WRBUF + RDBUF, 1/2/4 data lines, 1/2/4 address lines (IDF spi_slave_hd protocol) | all ok, both directions | quad write ok |
| quad write 32 B @ 10 MHz | **2902 cycles** | 5754 cycles |
| octal write 16 B (core 1 samples the 8 pads at 78 kHz) | 16 clocks, 0 bytes wrong | - |

Slave-HD init makes SCLK/CS inputs: when master and slave share pads, init the slave first.

## UART modes (`periph_test` uart_modes vs `bench/idf_uart_modes`, C9:D0, jumper 12->13, pads 16/17)

| | esp-ndf | ESP-IDF |
|---|---|---|
| HW flow control (RTS pad looped to CTS, RX ISR off) | FIFO held at 64 (threshold), no overflow, 300/300 B after | - |
| RS485 half duplex: DE high while sending, echo dropped | yes, 0 echo bytes | yes, 0 echo |
| RS485 DE release after stop bit, 1 Mbaud | **669 cycles** | 1311 cycles |
| RS485 collision detect: own echo / 2nd driver on RX | 64/64, 0 clashes / 64 clashes | - |
| IrDA SIR UART2 -> UART1 | 64/64 B | - |
| pattern '+++' in "hello+++world" | position 5 | position 5 |

IrDA is half duplex: the sender needs irda_tx_en (set while sending, cleared on TX_DONE), and its own receiver is
blind meanwhile, so a pin loop on one UART cannot work (irda_dplx is an internal loopback). Flow control: configure
CTS before RTS when they share a pad.

## I2C extended (`periph_test` i2c_ext vs `bench/idf_i2c_ext`, C9:D0, jumpers 10-11 / 12-13)

| | esp-ndf | ESP-IDF |
|---|---|---|
| 10-bit 0x2A5 write 8 / read 4, other 10-bit address NACKed | ok / ok / NACK | ok |
| write after a NACKed foreign 10-bit header | ok | ok |
| 10-bit write 8 B @100 kHz | **221.6k cycles** (floor 220.8k) | 230.5k cycles |
| SCL stretched 20 ms mid-byte, timeout asked 2 ms (set 3.3 ms) | TIMEOUT after 6.6 ms | - |
| slave holding SDA (core 1 emulation) | freed after 3 clocks | - |
| two masters started together, x10 | exactly one loser every round, loser retries -> NACK | - |

Found on the way: (1) the S3 I2C fsm_rst does not clean the controller (IDF soc_caps says so): after a NACK
the next 10-bit header was NACKed until the module was reset and re-timed, as IDF does; (2) init must reset
the module and mask the old role's interrupts (slave -> master re-init hung in an interrupt storm); (3)
arbitration_en (reset default) was being cleared by our CTR write; (4) the filter threshold field is
SCL [3:0] / SDA [7:4], we had set SCL only.

## RMT extended (`periph_test` rmt_ext vs `bench/idf_rmt_ext`, C9:D0, jumper 10 -> 11)

| | esp-ndf | ESP-IDF |
|---|---|---|
| TX loop 3 items x 100 (hardware loop count, auto stop) | 300 pulses received | - |
| ch0/ch1 start skew, without / with TX sync | 369 / **0** cycles | - |
| RX demodulation, 5 bursts of 38 kHz x 600 us | 115 raw items -> 5 items, 621 us | - |
| TX DMA (ch 3) 1000 items / RX DMA (ch 3) 1000 items | all within 1 tick / all within 1 tick | - |
| WS2812 3 LEDs encode + send call / until done | **2287 / 23393 cycles** | 3462 / 26956 cycles |

DMA (as IDF): the channel RAM stays in the path as a ring (wrap on both sides), TX prefills ~1 us before
start, RX limit 0. RX demod thresholds are maxima: a carrier at the nominal duty needs margin (+25 %).

## MCPWM extended (`periph_test` mcpwm_ext vs `bench/idf_mcpwm_ext`, C9:D0, jumpers 10 -> 11, 12 -> 13)

| | esp-ndf | ESP-IDF |
|---|---|---|
| fault pin, one-shot: tripped / after fault gone / released | 0 / 0 (latched) / running | - |
| fault pin, cycle by cycle: tripped / after fault gone | 0 / running by itself | - |
| carrier 1.25 MHz, 4/8 duty on 1 kHz 50 % PWM, 10 ms | 6250 pulses (ideal 6250) | - |
| capture prescale 4 on 1 kHz | **4 periods** (320000 ticks) | 6 periods (480000) |
| compare update | **3 cycles** | 76 cycles |

Found: (1) the capture prescaler is not "field + 1": field f >= 1 captures every 2f-th edge (measured n = 2..8;
IDF's prescale = 4 gives 6) - the driver maps n to f = n/2; (2) re-timing a running timer kept the old timer
prescale until a software sync reloads it.

## PCNT extended (`periph_test` pcnt_ext vs `bench/idf_pcnt_ext`, C9:D0, CPU-made quadrature 12/10 -> 13/11)

| | esp-ndf | ESP-IDF |
|---|---|---|
| quadrature +100 / -40 | 100 / 60 | - |
| watch points thres 25 / -10 / zero | events at 25 and -10, zero cross | (watch points) |
| step notify 10 (35 up, 50 down) | 10 20 30 20 10 0 -10 | not on S3 |
| index Z: latch + reset | 77 latched, +5 after -> 5 | not on S3 (no clear signal) |
| read count | **91 cycles** (32-bit) | 310 cycles |

Thresholds and limits latch only at a counter reset (as IDF notes): re-arming keeps the total in a software
offset. Step notify uses the wrap limit itself (= step), so it stays gap-free.

## LEDC gamma fade (`periph_test` ledc_gamma vs `bench/idf_ledc_ext`, C9:D0, pad 4)

0 -> 1024 (10 bit, 20 kHz) in 160 ms, gamma 2.2, measured duty every 10 ms (per mille):
1 10 26 46 74 113 161 211 274 339 429 525 622 720 817, end 1000 after 162 ms
(ideal (t/160)^2.2: 50 ms 77, 100 ms 356, 150 ms 868). 16 hardware linear segments chained from the
fade-end interrupt; IDF on the S3 has linear fades only. Duty update: **38 cycles** vs IDF 765.
Found: the fade step (duty_scale) is in whole duty units; our ledc_fade had used 16 per step.

## Touch extended (`periph_test` touch_ext vs `bench/idf_touch_ext`, C9:D0, pad 10)

| | esp-ndf | ESP-IDF |
|---|---|---|
| filter (IIR 16, smooth IIR 2) raw / smooth / baseline | 55152 / 55160 / 55156 | - |
| threshold 5 %, "touch" = pad slope 7 -> 6 (+16 % raw) | 1 touch event pad 10, 1 release event | - |
| proximity pad 10, 50 scans | count 49 | - |
| raw read | **99 cycles** | 532 cycles |

A driven neighbour pad (jumper) does not move the count: the stimulus is the charge slope instead. The RTC
interrupt is now shared (soc/esp32s3/rtc_irq.c: brownout + touch); bod_test still trips and restarts.
Denoise was tried and dropped: with only the enable bit the data freezes (needs the reference channel setup).

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

## NVS recent-item cache, gpt_get32

NVS keeps the last 8 found items in RAM (flushed on any write, erase, GC or
mount, so a hit is always the newest copy). Full suite passes (600 updates
through GC, remounts, encrypted, wrong key, IDF data):
nvs_get_u32 1 747 -> **498** cycles (IDF 26 269), encrypted 4 711 -> **435**
(IDF 82 455).

GPTimer read profiled (examples/gpt_probe): latch write 6, latch wait ~50
(one extra poll), LO+HI reads 47 -> gpt_get 110-124 is the hardware floor.
Added gpt_get32 (low word only): 100 cycles.

## FAT path cache

4-entry cache of path -> (parent, directory entry) in resolve(), flushed by
create, entry update, unlink, mkdir, format and mount. fat_open no longer reads
the directory sector just to learn which sector holds the entry (dent_sec()
computes it from the FAT chain). Repeated open of the same path: 51 206 ->
**814** cycles (IDF 70 187). First open of a path still costs one directory
sector read (~51 000). Full suite passes (create/overwrite/truncate/unlink/
mkdir/remount/IDF volumes/FAT-mirror repair).

## int16 vector kernels, software pipelined

Every vector operand is now loaded two instructions before its use (unroll
by 2, register rotation, prologue/epilogue without over-read).

| 1024 elements | before | now | instruction floor | ESP-DSP |
|---|---|---|---|---|
| int16 dot product | 405 | **280** | ~260 | 1 073 |
| int16 saturating add | 535 | **409** | ~390 | 554 |

All lengths 16..512 (step 16) equal to C including saturation and a guard
word after the output: 0 mismatches (accelerated and NO_ACCEL builds).

## memcpy, different alignments: fused shift+load

dsp_copy128u now sets SAR_BYTE once (EE.LD.128.USAR.IP) and then runs
EE.SRC.Q.LD.IP (shift + load next block) with three rotating registers: 2
instructions per 16 bytes. 8 KB different alignment: 2 440 -> **1 443** cycles
(1 362 MB/s). Exhaustive sizes 0-300 x offsets 0 wrong; ISR path ok.

## Flash reads through the data cache (examples/flash_mmap)

Steps measured, 4 KB cold read, data checked against the SPI1 read each time:

| configuration | time |
|---|---|
| SPI1 register path (previous best) | 209 µs |
| cache as the ROM leaves it (SPI0 DIO, 16-byte lines) | 336 µs |
| SPI0 switched to QIO (cmd 0xEB, 6 dummy) - with the old 0xBB command the data was wrong | 220 µs |
| + 32-byte lines | 161 µs |
| + 64-byte lines | **132 µs** |
| + cache preload engine | 2 218 µs (not used) |
| + autoload (sequential prefetch), sizes 0-3 | 130-134 µs (no gain, not used) |

flash_read() now copies from 0x3C000000 + addr (whole 16 MB mapped, 64 KB
MMU pages); repeated reads hit the cache (4 KB in 13 µs). Writes/erases stay
on SPI1 and invalidate the written range in the cache. Without quad mode, or
with SOC_HAS_FLASH_CACHE = 0 (NO_ACCEL), the SPI1 path is used. CRC of 4 KB
= host file; full suite passes. A/B on the same chip (alternating builds):
SPIFFS 40 x 30 KB rewrites 8 413 / 8 397 ms (cache) vs 8 327 / 8 358 ms (SPI1)
- the read path is not what limits it; the chip's write time is.

## Core services: time base, software timers, heap, log (2026-10-18)

examples/core_test (DEVKIT) vs bench/idf_periph [core]:
- time_ticks()/time_us(): 149 cycles (IDF esp_timer_get_time 193). Hi/lo re-read guards the other core latching between the two reads; monotonic over 100k reads.
- swtimer 1 ms periodic for 1 s: 1000 calls, span exactly 999 x 16000 ticks, deadline->callback 26 ticks fixed (1.6 us). IDF esp_timer: 999 calls, period 981..1000 us, drift -19 us.
- 32 concurrent timers (100..3200 us) counts within +-1; stop-from-own-callback; start from core 1 (fires on core 0, the ISR owner).
- heap: 16-byte aligned, boundary tags, whole region merges back; malloc+free 64 B 165 cycles (IDF 1346). 346 688 B free with 16 KB stacks.
- delay_ms(250) = 250 001 us (was 250 046 when looping delay_us(1000)); delay_us safe beyond 17.9 s (chunked).

## Runtime library round: float, libc, ring buffer, async DMA, watchdog, panic (2026-10-18)

- Float helpers (libgcc is windowed, unusable): `__divsf3`/`sqrtf` on the FPU helper instructions (same sequence as the ROM, CALL0): 56 / 58 cycles vs IDF 75 / 118. Portable versions (integer long division; Newton + residual choice) kept and tested. Conversions float<->double, int64<->float, 64-bit shifts. examples/libc_test: 1 725 numpy IEEE vectors bit-exact for divide and sqrt (both implementations), 1 500 each for double->float and int64->float.
  - First divide (Newton in float) missed 6/1725 by 1 ulp at the normal/denormal edge -> replaced.
- printf family: snprintf/vsnprintf/sprintf/puts/putchar, %f %e %g (+ precision, + and space flags, %.Ns), round-half-even on exact ties like glibc; 39 reference formats match C. %.3f 4 575 -> 1 577 cycles by generating only prec+1 digits (IDF 11 862).
  - Zero hung the tiny-value scaling loop (mantissa 0) -> early return.
- stdlib: strtol/ul/ll/ull (saturating, base 0/2..36), atoi/atol/atoll, strtof/strtod (float precision), qsort (316 cycles/element on 2 000 ints vs IDF 415), bsearch, abs.
- ringbuf.h: SPSC byte ring, contiguous items, zero-copy both sides. 16 B put+get 303 cycles, zero-copy 105 (IDF xRingbuffer 1 102 / 1 237). Core1->core0 20 000 variable items through 1 KB: intact, 1 107 cycles/item.
- gdma_memcpy_async: start 1 404 cycles, 65 472 B done in 247 975 cycles (63 MB/s), CPU free meanwhile (IDF esp_async_memcpy: start 56 756, 51 MB/s). Per-channel descriptor sets (the blocking copy shared one set across channels).
- Watchdog (TIMG0 MWDT): feed 27 cycles (IDF esp_task_wdt_reset 362). Report stage: level-3 interrupt prints the stalled PC + backtrace, stage 1 resets; next boot reads "TG0 watchdog (system)".
- Panic: registers a0-a15 (a12-a15 saved into the free frame words), SAR/loop regs, stack-scan backtrace (return addresses that follow call0/callx0), `make backtrace`, NDF_PANIC_RESTART. examples/sys_test runs 3 boots: watchdog report -> TG0WDT reset -> fault report -> software restart; traces decode to the exact source lines.
- BUG FIXED: level 2-5 interrupts (and exceptions) run with PS.EXCM = 1, which disables zero-overhead loop-back: every C loop in a level 2-5 handler executed its body once (printf in a level-3 ISR printed 1 char, stack buffers not zeroed). Entry now writes PS = UM | level early in the register save (no rsync needed); fault paths clear EXCM before C. Level 3 entry 66 (was 63-67), level 5 C 53 / lost 115 (was 46-49 / 109).
- periph_test: RAM 5 KB over after this round -> its core-1 stack set to 8 KB via app.mk (menuconfig).

## Closing the runtime gaps: doubles, exact conversions, exact backtrace (2026-10-18)

- Double arithmetic (FPU is single precision; libgcc unusable): lib/double.c (general, IEEE-exact: add/sub/mul/div, compares, int conversions) + lib/double.S fast paths for add/sub/mul on normal operands.
  - Per call vs IDF ROM (function pointer, minus a dummy call): add same sign 50 (51), different sign 48 (52), gap 33 39 (45), cancellation 68 (74), mul 77 (83), div 271 (526).
  - Path to there: C 284/277/1270 -> inline funnel shifts 271 -> C fast paths 148 -> asm 106 -> guard word, no slow path after entry 74 -> packed-accumulate (y's mantissa added into packed x; carry fixed by (S >> 1) + (top12 + 1) << 19) 60 -> mirrored macros, no swap 57 -> branch relaxation found (8-bit compare branches to far targets became branch + jump: +6 taken branches) and removed 39-57 -> inline exits 48-50.
  - Exactness: 844 numpy vectors per operation + 200 000 random pairs per operation (gaps 0..64, cancellations, range edges) against the general C code: 0 mismatches.
- strtod/strtof (lib/strtod.c): correctly rounded for every input. Clinger exact fast path; otherwise 64-bit approximation from a table of normalised powers of ten (tools/gen_pow10.py) and, only when within 32 units of a halfway point, an exact big-integer comparison (lib/bigint.c, up to 800 digits + sticky tail). Hex floats, inf/nan(...). 438 reference strings (hard cases, exact halfway points written in full with 800-digit tails, denormals, overflow) for both double and float: 0 mismatches. strtof rounds the decimal directly (no double rounding).
- printf: exact decimal digits for every double at any precision (64-bit path when exact, big integers otherwise, ~2 KB stack only for those values); round half to even on exact ties; %a/%A (hex floats, rounded), %o, %n, # flag, hh/h/j/L modifiers, %p as 0x...; 68 + 10 + 1 reference cases match C.
- Exact panic backtrace: tools/mkunwind.py turns the ELF's .debug_frame (DWARF CFI) into a compact table appended to app.bin after the ROM image (flash only, 0 RAM); arch/xtensa/unwind.c walks frames with it, crossing interrupt frames through EPCn and the saved frame. sys_test: watchdog trace = report -> wdt_isr -> _l3_entry -> stalled loop (main.c:107) -> main -> start; fault trace = main.c:118 -> start. No stale entries. Stack scan kept as fallback for images without the table.
- periph_test: core-0 stack 12 KB (app.mk) to fit the larger runtime.

## USB OTG full-speed device, CDC-ACM (`examples/usb_cdc` vs `bench/idf_usbcdc`, 2026-10-24)

The IDF sibling is its own project (not `bench/idf_periph/t_usb.c`): TinyUSB takes the shared USB PHY away from the
USB-JTAG console and needs the `esp_tinyusb` component, like the RF siblings. Same board, port and host script
(`tools/usb_cmp.sh`); both at -O2 / 240 MHz, IDF at its full-performance settings (see PERFORMANCE.md).

| | esp-ndf | ESP-IDF TinyUSB |
|---|---|---|
| enumerates as CDC-ACM, echo 372 B | OK | OK |
| download 4 MB, byte-exact | **1152 KB/s** | 867-870 KB/s |
| upload 4 MB, byte-exact, device-side count | **994-995 KB/s**, 0 errors | 505 KB/s, 0 errors |
| USB CPU while moving data | **~0.9 %** | ~25 % of core 0 |

Functional results agree (enumeration, echo, exact data both ways, device counts). Buffer-DMA driver.

## Code from flash (XIP) and boot (`examples/xip_test`, `examples/boot_time` vs `bench/idf_xip`, `bench/idf_boot`, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| same kernels in RAM and flash give the same result | yes | yes |
| flash erase + write while code runs from flash, readback | exact, core 1 stalled meanwhile | exact |
| libc_test / sys_test with libraries in flash | ALL PASS (watchdog report, fault backtraces from flash code, restarts) | - |
| reset -> main, cold path (ROM + our loader) | 6.18 ms | 35.5 ms |
| reset -> main, warm (software/watchdog reset), no ROM code | **0.81 ms** (XIP=lib 0.85) | 35.5 ms |
| esptool still connects with the RTC stage installed (its reset goes to the ROM) | yes | yes |


## Flash driver (`examples/flash_test`, `bench/idf_flash`, 2026-10-24)

| | esp-ndf | ESP-IDF |
|---|---|---|
| erase 4/32/64 KB + 256 KB region, write 4/64 KB, read back exact | PASS (5 boots at 120 MHz) | data ok |
| chip detected | BY25Q128, SFDP 9 dwords, erase 4/32/64 KB | boya |
| unique ID | 50442159204a141c | 50442159204a141c |
| QE cleared (esptool write-flash-status 0): boots DIO, driver sets QE, next read QIO | yes (status 0x0000 -> 0x0200) | - |
| auto-suspend forced on BY25Q128 | erase never resumes (hardware) | hangs (same) |
| flash code (cache miss every call) on core 1 during erase 64 KB + write 4 KB | correct results, paused during the erase | - |
| warm boot after the changes | 782-787 us | 35.5 ms |
| periph_test (XIP=all now: fits) | flash, partition, nvs, gpio, gptimer, mcpwm, pcnt, sdm, gdma PASS; loopback tests (uart, i2c, rmt, spi, i2s, ...) receive nothing: devkit jumpers GPIO10-11 / 12-13 not fitted at test time | - |

## RNG (`examples/rng_test`, 2026-10-24)

| | result |
|---|---|
| ChaCha20 RFC 8439 2.3.2 known answer | PASS |
| rng_hw 4 KB: ones / runs / chi2 | 0.4952-0.5016 / 0.4987-0.5047 / 251.9-262.9 PASS |
| rng_fill 64 KB: ones / runs / chi2 | 0.4999-0.5003 / 0.4999-0.5002 / 199.3-214.0 PASS |
| two 32-byte draws differ | PASS |

## SHA (`examples/sha_test`, 2026-10-24)

NIST vectors PASS: SHA-1 / 224 / 256 / 384 / 512 / 512-224 / 512-256 "abc", SHA-256 empty and 448-bit, SHA-1 448-bit,
SHA-256 and SHA-512 one million 'a' (1000 updates). Split + interleaved (two contexts) + unaligned updates equal the
one-shot digest. Same results with the DMA threshold at 2, 4, 8, 16 blocks.

## OTA (`examples/ota_test`, real resets, XIP=lib, 2026-10-24)

| step | result |
|---|---|
| factory copies itself into ota_0 (RAM image + code window), ota_end SHA-256, ota_set_boot | PASS |
| ota_0 first boot: PENDING_VERIFY, ota_mark_valid -> VALID | PASS |
| ota_1 installed, booted, not confirmed, reset | loader marks ABORTED, boots ota_0 (VALID): PASS |
| corrupted image: ota_end rejects it; booted anyway | loader marks INVALID (SHA-256), stays on ota_0: PASS |
| otadata erased | factory boots: PASS |
| IDF otatool switch_ota_partition --slot 1 | our loader boots ota_1 |
| our ota_set_boot(ota_0) | IDF otatool reads seq 1, crc 0x4743989a (valid) |

## Watchdogs (`examples/wdt_test`, 2026-10-24)

| test | result |
|---|---|
| RTC WDT 100 ms fed every 50 ms for 1 s | no reset: PASS |
| RTC WDT 200 ms starved, system reset | cause 9 RTC watchdog (system), 201 ms: PASS |
| RTC WDT 200 ms starved, chip reset (RTC domain too) | cause 16 RTC watchdog (RTC): PASS |
| TG1 MWDT 100 ms starved | cause 8 TG1 watchdog, 101 ms: PASS |
| super WDT armed, not fed | cause 18 super watchdog: PASS |
| reset cause after the warm stage hands a reset to the ROM | the real cause (e.g. 21 USB-UART), not the internal software reset |

## Brownout (`examples/bod_test`, 2026-10-24)

| test | result |
|---|---|
| level 7 (2.44 V, boot default): no detection in 5 s | PASS |
| level 1 (3.30 V, at this board's 3.3 V supply): detected, interrupt ran, restart | cause 15 brownout: PASS |
| hardware reset mode (BROWN_OUT_RST_ENA), level 7 | stable 120 s + port close/reopen (C9:D0): now the boot default. The earlier "~4.5 s loop" was not the detector: in those boots its register read the reset defaults (reset off); the resets were cause 21 USB-UART resets, i.e. the host (ModemManager probing re-enumerated ports) |
| analog threshold kept through resets (power-on value 0), brownout register not | register set every boot, threshold only when it changed (RTC note) |

## RTC clocks (`examples/rtcclk_test`, 2026-10-24)

| test | result |
|---|---|
| RC slow clock calibrated (timer group 0, 256 cycles, 1.7 ms) vs RTC timer over CPU time | 149 001 vs 149 020 Hz: PASS |
| switch to 8 MHz / 256: calibrated vs RTC timer vs rtc_slow_hz | 78 574 / 78 539 / 78 557 Hz: PASS |
| back to RC | 148 900 Hz: PASS |
| 32 kHz crystal start, none fitted | -1 after 400 ms, select refused, stays on RC: PASS |
| RTC WDT on the calibrated clock (wdt_test) | 200 ms -> 201 ms |

Not run: 32 kHz crystal / external oscillator and its watchdog (no crystal on this board).

## Memory map, heap, PSRAM (`examples/mem_test`, 2026-10-24)

| test | result |
|---|---|
| RTC_DATA_ATTR initialised, RTC_BSS_ATTR zero at boot | PASS |
| after a software reset: RTC_DATA re-initialised, RTC_BSS zeroed, RTC_NOINIT kept | PASS |
| PSRAM 8 MB into the heap; malloc(100000) in PSRAM, malloc(100) and DMA caps internal, SPIRAM caps in PSRAM | PASS |
| data in PSRAM blocks, all frees return both regions to their start | PASS |
| PSRAM left with another read latency (MR0 kept through resets) | psram_init now finds it (latency search) |
| flash at 120 MHz + PSRAM | psram_init returns the flash to 80 MHz first (shared MSPI core clock, as IDF) |

## Flash on the BOX-3 (GD25LQ128, 2026-10-24)

flash_test PASS x3: chip detected (QE method 16-bit status, SFDP 16 dwords, suspend from SFDP), 120 MHz tuning, erase /
write / read exact, flash code on core 1 correct during erase + write with hardware auto-suspend for misses every
0, 20, 50, 100, 200, 500, 1000 us (hold 100 us).

## eFuse fields (`examples/efuse_dump`, C9:D0, 2026-10-24)

All 232 rows of ESP-IDF's esp_efuse_table.csv (tools/gen_efuse.py; multi-row fields such as MAC joined in IDF order),
read by name. Against `espefuse.py dump` + `summary --format json` on the same chip: **100 of 100 positioned fields
equal the raw bits** (12 espefuse entries have no bit position to compare). Read-only: no burning code.

## AES modes (`examples/aes_test`, 2026-10-24)

NIST SP 800-38A F.1-F.5 (ECB, CBC, CFB128, OFB, CTR; AES-128) PASS by DMA, block by block and decrypting. AES-256,
64 KB random data, every mode: DMA output and final IV equal the CPU path, decryption restores the plaintext.

## RSA / big-number accelerator (`examples/rsa_test`, 2026-10-24)

RSA-1024/2048/4096: x^65537, x^d and x*y mod M all equal Python pow() vectors (bench/rsa/gen_vectors.py) on C9:D0;
IDF sibling bench/idf_rsa gives the same results. Timings in PERFORMANCE.md.

## P-256 ECC (`examples/p256_test`, 2026-10-24)

Public key, ECDH shared secret, ECDSA sign with the RFC 6979 A.2.5 k (r,s match the RFC) and verify of a python
cryptography signature all PASS on C9:D0 (vectors bench/p256/gen_vectors.py); random keygen/sign/verify round trip
passes, tampered signature and off-curve key are rejected. Same results from bench/idf_p256.

## AES-GCM (`examples/gcm_test`, 2026-10-24)

8 cases (AES-128/256, IV 12 and 16 bytes, AAD 0-20, 0 B..16 KB): tag and first ciphertext block equal python
cryptography (bench/gcm/gen_vectors.py), decrypt in place restores the plaintext, a flipped tag bit is rejected.
Found on the way: aes_block ran before aes_init on a first call and cached the key by pointer (a reused key buffer
kept the old key); both fixed in aes_block, aes_test still PASS.

## X25519 (`examples/x25519_test`, 2026-10-24)

Public key and shared secret equal python cryptography (bench/x25519/gen_vectors.py), RFC 7748 5.2 vector 1 PASS on
C9:D0; P-256 test re-run after the move to lib/ecc.c still PASS. Same results from bench/idf_x25519.

## Cross-core call and stall (`examples/xcore_call`, 2026-10-24)

10 000 xc_call round trips from core 0: each result came from core 1 with the right argument. xc_stall(1): core 1's
counter did not move for 2 ms; after xc_unstall it counted again. PASS on C9:D0.

## Floats in interrupt handlers (`examples/fpu_isr`, 2026-10-24)

Core 0 runs a float chain kept in FP registers while core 1 storms a level-1 handler (float loop) and a level-3 or
level-5 handler (sqrtf) that preempts it: main result bit-equal to the run without interrupts, 0 wrong handler results
in ~4400 L1 + ~8400 L3/L5 runs. Bug found on the way: the trap overwrote EPC1, so the level-1 exit returned into the
handler; EPC1 is now kept in the level-1 frame. BR (FP compare results) is part of the saved state. periph_test (21),
core_test, irq_latency, xcore_notify re-run: PASS.

## Profiling and debug (`examples/debug_test`, `examples/stack_overflow`, 2026-10-24)

Perf counters: cycles equal CCOUNT (+-2), 10 005 instructions for 10 000 nops + loop setup, 1001 loads for 1000.
Write watchpoint: callback with the store's PC inside poke(), store completed, second store not caught (one-shot).
Stack guard: recursion crossing a limit 3 KB down raises the callback with a PC inside deep(); with the boot guard,
runaway recursion ends in "*** PANIC core0: stack overflow (SP left its stack)" + backtrace. Found: ASSIST_DEBUG needs
RLS as well as ENA to raise the interrupt (RAW was set, no interrupt); perfmon KRNLCNT=1 means "only above
TRACELEVEL" (counted nothing).

## GDMA: PSRAM, priority, errors (`examples/gdma_ext`, 2026-10-24)

1 MB internal->PSRAM, 64 KB PSRAM->internal, 1 MB PSRAM->PSRAM by DMA: data checked byte for byte. Priority 5 vs 0:
the higher channel finished first both ways. Unaligned PSRAM request refused; owner=0 descriptor with owner check
-> GDMA_ERR_OUT, peripheral-space buffer -> descriptor error; gdma_reset recovers, next copy correct. PASS on C9:D0.
Board note corrected: C9:D0's PSRAM is octal (IDF quad mode fails with ID 0xffffff).

## Memory protection (`examples/memprot_test`, `examples/memprot_exec`, 2026-10-24)

Store into code through the DRAM alias and through the IRAM address: blocked (code bytes unchanged), reported with
bus, address and write flag; data stores and calls unaffected; memprot_code_write opens and recloses code. Jump into
a data buffer: "illegal instruction ... PC in data: fetch blocked by memprot". With protection on from boot:
periph_test 21/21, debug_test, gdma_ext, flash_test, ota_test, fpu_isr, xcore_call, irq_naked (vector patch), core_test
PASS. irq_naked's exit figure had wrapped (time read before the flag check); fixed: 14 / 12 / 33 cycles.

## PIE in interrupt handlers (`examples/pie_isr`, BOX-3, 2026-10-24)

Main loop dsp_dot_s16 over 4096 elements x 20 000 while level-1 and level-3 / level-5 handlers run dsp_dot_s16 and
a float op: 0 wrong results in main and in ~4700 + ~8700 handler runs per level; fpu_isr still PASS. Found: a second
cpu1_start tripped core 1's stack guard (the ROM start stub runs on its own stack) -> panic; cpu1_start now disarms
core 1's guard, sys_start1 re-arms it.

## DSP library (`examples/dsp_lib`, BOX-3, 2026-10-24)

26 checks PASS: complex float FFT 256/1024/4096 (rel. error < 1e-5 vs DFT), rfft, int16 FFT (<= 16 LSB of DFT/n:
PIE truncation bias, bit-identical to ESP-DSP), windows (Hann exact, flat-top = ESP's normalised coefficients),
FIR 64 taps, decimating FIR, biquad, vector ops, int16 add/mul, matmul f32/s16, conv, corr, DCT-II (vs direct sum),
Kalman (2-state constant velocity from noisy positions: v 0.5025 for 0.5). Bugs on the way: twiddle table only
4-byte aligned broke EE.LDF.64 (NaN); EE.LDF.64.XP stepped before the load; fmax() hid NaNs in the check.

## int8 NN layers (`examples/nn_lib`, BOX-3, 2026-10-24)

10 layer types bit-exact with ESP-NN (checksums equal): conv 1x1 and 3x3 pad 1, depthwise 3x3 stride 1/2, FC with
input offset, max/avg pool 2x2, add with input rescales, relu6, softmax. The end-to-end model comparison was cut
short: the IDF sibling's new static buffers starved its scratch allocation, ESP-NN panicked and the IDF default
(panic -> reboot) produced a reboot storm that took the BOX-3 off the host's USB. Fixed in code (checked heap
allocations, buffers reused) and every bench/idf_* sdkconfig.defaults now sets CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT.

Rerun after the USB recovery (2026-10-24): layers reproduce; the tiny model end to end is bit-exact with ESP-NN
(checksum -2971 both) at 1.17x. Bugs found by it: the conv kernel's generic path fell through into the k16 = 9 code
after its last channel (counter underflow -> load fault; our halt-on-panic kept the board reachable), and FC rows
longer than the 4608-byte splat buffer took the scalar path although the kernel reads only 16 splat bytes.

## Fast math + Q15/Q31 (`examples/fastmath_test`, BOX-3, 2026-10-24)

Error sweeps against double precision PASS the bounds in fastmath.h; Q15/Q31 edge cases (saturating -1 * -1, add
overflow, conversions at +-1) PASS. The first IDF numbers were 0 cycles: GCC folded the unused calls; outputs are
volatile now on both sides.

## ULP-RISC-V + ULP-FSM (`examples/ulp_test`, BOX-3, 2026-10-24)

ULP-RISC-V program (bench/ulp/ulp_count.S) restarted by the ULP timer every 10 ms: 98 runs/s, magic word written,
mailbox answered (41 -> 42), frozen after ulp_stop. ULP-FSM program (ulp_fsm_count.S): 99 runs/s, frozen after stop.
Found: (1) the ULP does not start with the RTC fast clock on XTAL/2 (our default) - ulp_run selects RC_FAST as IDF
does; (2) a program must clear COCPU DONE / SHUT_RESET_EN at its start (IDF start.S does), else the timer never
restarts it (and later programs fail too - IDF's own ulp_riscv_run included). ulp_load switches the CPU reset vector
to the ROM (the warm-boot jump shares 0x50000000) and the loader halts any running ULP before touching RTC memory.

## CPU frequency locks (`examples/pm_test`, BOX-3, 2026-10-24)

pm_config(240, 80): 80 MHz without a lock, 240 MHz with one, still 240 with a nested lock released, 80 after the last
unlock (ccount vs systimer); delays exact at 80 MHz; lock count back to 0. IDF's DFS measured on the same board keeps
the CPU at 240 MHz while a task runs (scaling happens in its idle task only).

## Flash suite on the BOX-3's GD25LQ128 (`examples/flash_test` XIP=lib, 2026-10-24)

JEDEC c86018, 16 MB, SFDP, quad (QE method 1), suspend 75/7a, 120 MHz: erase / write / read all PASS with data
checked; with core 1 running flash code during a 64 KB erase + 4 KB write the longest core-1 pause is 215-284 us
(suspend), no wrong data. bench/flash_box3_result.txt.
