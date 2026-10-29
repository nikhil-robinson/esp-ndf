# Firmware audit findings (esp-ndf, 2026-10-25) - code level, ALL FIXED (status at the end)

Tools: GCC 13 -fanalyzer, cppcheck 2.17, clang 19 analyzer, clang-tidy (cert/bugprone), semgrep (0 hits), flawfinder,
host ASan/UBSan fuzzing (tools/fuzz/fuzz_fat.c, tools/fuzz/fuzz_flashfs.c, tools/test_pfs.c), manual review of every
trust boundary in AUDIT_MAP.md.

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| C1 | CRITICAL | components/drivers/usb/usb_msc.c:108 | READ/WRITE(10) range check `lba + cnt > blocks()` can wrap | `lba > blocks() \|\| cnt > blocks() - lba`; bounds checks in every bdev |
| C2 | CRITICAL | components/drivers/storage/fat.c:212-220 | LFN sequence number 0 gives a negative index into a stack buffer (fuzz + clang confirmed) | reject ord < 1 or > 20 |
| H1 | HIGH | components/drivers/storage/fat.c:283-299, 439, 760 | resolve_walk leaves outputs unset when a parent dir is missing; open(CREAT)/rename use them | initialise outputs, distinct error, callers check |
| H2 | HIGH | components/drivers/storage/fat.c:626 | fat_readdir copies a name of up to 255 B into a caller buffer of unknown size | add a size argument (fat/spiffs/pfs/vfs readdir) |
| H3 | HIGH | components/lib/heap.c:107-120 | free() does not validate the pointer / in-use flag (double free, stray pointer) | header magic + used check, panic on mismatch |
| H4 | HIGH | components/drivers/flash/flash.c:426,573,580; components/drivers/storage/partition.c:29-31 | `addr + len` range checks can wrap; partition entries not validated against flash size or the image area | overflow-safe checks; reject bad entries |
| H5 | HIGH | components/drivers/storage/ota.c | OTA image has an integrity hash, no signature: any writer can install firmware | ECDSA P-256 signature check before switching slots |
| H6 | HIGH | Makefile | no stack canaries (-fstack-protector-strong) | add flag + __stack_chk_fail -> panic |
| H7 | HIGH | components/soc/esp32s3/sys.c | no watchdog armed at boot; panic default halts | default watchdog, documented safe-state policy |
| M1 | MEDIUM | components/lib/heap.c | block headers without canary: overflows corrupt metadata silently | canary checked on free/alloc |
| M2 | MEDIUM | components/drivers/storage/partition.c | MD5 entry optional; table accepted without it | require MD5 (configurable) |
| M3 | MEDIUM | components/soc/esp32s3/coredump.c | 8 KB of raw stack written to flash in plaintext (secrets) | option to omit the stack / clear key buffers |
| M4 | MEDIUM | components/lib/nn.c:17, 303 | model parameters can cause divide by zero / shift UB (panic) | validate model parameters at load |
| M5 | MEDIUM | system | JTAG + console always open (no production lockdown; eFuse work excluded by owner) | accepted risk, documented; app-level console lock option |
| L1 | LOW | components/lib/ringbuf.c:12 | buffer alignment not enforced (alignment exception) | require/align to 4 |
| L2 | LOW | components/drivers/adc/adc.c:130 | analyzer: possible uninitialised read | initialise d[] |
| L3 | LOW | components/drivers/flash/flash.c:236,252 | SFDP shift UB on bad chip data | range check |
| L4 | LOW | components/lib/bigint.c:93; components/soc/esp32s3/rtcclk.c:31 | negative shift / unchecked enum index | argument checks |
| L5 | LOW | components/soc/esp32s3/system.c:65 | printf format/type mismatch | fix formats |

False positives checked and dismissed: comparePointers on linker-section loops (cmd.c, unit.c, sys.c), ota.c:56
uninit, nvs.c:406 and heap reg[] bounds, 213 "use memcpy_s" advisories, -Wcast-align sites
(all guarded by alignment tests except those listed).

## Status (pass 2, 2026-10-25)
Every row above is fixed (commits 4cd296e, 7c51a3f, be47436 and later); M5 is the accepted eFuse risk (THREAT_MODEL.md).
Re-scan after the fixes (cppcheck --enable=warning, clang analyzer, semgrep p/c) found and fixed:

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| P1 | MEDIUM | components/drivers/lcd/i80.c lcd_rgb_start | a zero-byte frame gave k = 0 and wrote fbd[-1] (pass 1 wrongly dismissed it) | `k <= 0` refused |
| P2 | LOW | components/drivers/flash/flash.c flash_info / oct_init | density codes 0x20/0x21 shifted by 32/33 (L3 fix incomplete); octal path unchecked | 0x20/0x21 mapped to 64/128 MB, everything else bounded |
| P3 | LOW | components/lib/double.c __fixdfsi | negating INT32_MIN in signed arithmetic | negation done unsigned |
| P4 | MEDIUM | components/lib/dsp_kalman.c | caller-set n/m larger than DSP_KF_MAX overflowed stack matrices | refused (predict returns, update -1) |
| P5 | MEDIUM | components/soc/esp32s3/sys.c ndf_panic | default halt-on-panic spun with interrupts masked, so the new interrupt watchdog reset the chip: a boot-time fault became a reboot loop | halt path stops the timer-group watchdogs; components/soc/test_apps/panic_halt_test |
| P6 | MEDIUM | components/soc/esp32s3/sys.c | no way to put actuators in a safe state on a fault | weak ndf_on_panic() hook, first thing in the panic path; crash_test checks it |
| P7 | HIGH | components/drivers/wdt/wdt.c wdt_isr | the ISR did not check its own interrupt status; with the interrupt watchdog on, it shares a level-3 line with other sources, so any of them (a memory-protection violation) ran it and caused a false "watchdog not fed" panic and reset (found by memprot_test; reproduced on 7c51a3f) | checks TIMG INT_ST bit 2 first |
| P8 | MEDIUM | components/drivers/xcore/xcore.c notify_isr | same pattern: on a shared line it ran the user handler without a pending request | returns unless FROM_CPU is set (+5 cycles) |
| P9 | MEDIUM | components/soc/esp32s3/link.ld.in | the last code word could sit exactly at the memprot split line; the instruction prefetch reads up to 16 B further, into the no-access area, and panics (debug_test, layout-dependent) | 16 B guard before the aligned split |
| P10 | LOW | components/drivers/gdma/gdma.c | M2M chain of 16 descriptors x 4032 B (whole cache lines) = 64512 B refused a 64 KB copy; the caller (a test) then spun for ever | 17 descriptors; sys_test no longer spins on a refused start |
| P11 | LOW | components/lib/string.c explicit_bzero | byte-wise volatile loop: wiping the 4 KB GHASH table cost 20 k cycles (small GCM messages 3x slower than before the hardening, slower than IDF) | memset + compiler barrier |
| P12 | MEDIUM | components/soc/esp32s3/link.ld.in XIP_ALL/XIP_LIB | code was placed in flash by matching object PATHS: an application under components/drivers/storage/ matched every object, boot code included -> reboot loop | patterns anchored to the SDK object tree (build/ndf/components/...) |
| P13 | MEDIUM | components/lib/ecc.c, rsa.c (design) | the fixes for fault injection (verify every signature) and RSA timing (engine constant-time mode) made P-256 sign 3.7x slower than before / slower than IDF and RSA private operations 34 % slower: a hardening that costs this much gets switched off in practice | P-256 rewritten (comba + NIST reduction, fixed-base comb): sign with the guard is 27 ms (IDF 99.6); the guard recomputes k G with n-k instead of verifying; RSA fast path default (leak analysed: a fixed key shows length / weight only), constant time kept as an option |
| P14 | HIGH | components/drivers/gdma/gdma.c | `gdma_alloc` / `gdma_free` / the M2M id selection updated shared bookkeeping without a cross-core lock; crypto takes a channel per operation on either core, so core 0 (SPI) and core 1 (AES / SHA) could be handed the same channel: wrong data on both, depending on boot timing (tests/stress failed deterministically once the boot timing shifted) | spinlock + interrupts off around all channel bookkeeping; ~95 cycles on every DMA-backed crypto call; tests/stress 60 s clean again |

Also fixed in pass 2: GDMA channel exhaustion (crypto DMA allocated per operation), M2M peripheral-ID collision with
AES DMA, WL state sanity before recovery (fuzz_wl), printf %n off by default, -Wformat-security, errata CACHE-126 and
LCD-239 workarounds (ERRATA.md). Remaining analyzer reports are reviewed false positives: signed `>> 31`
(implementation-defined, GCC arithmetic shift), `%lu` with uint32_t (unsigned long on Xtensa), loader/ota arrays
filled byte-wise or by slots(), the FIR read-behind window (documented taps-1 history, callers pass it), dead stores.

## Pass 3: wireless (BLE host, Wi-Fi, lwIP, TLS), 2026-10-29

Static review of the new components plus the hardware behaviour found on the bench (these are code-level findings; the live behaviour was checked with two-board tests, see docs/WIRELESS.md).

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| W1 | HIGH | components/drivers/rf/rf_wifi.c rf_wifi_tx_init | the MAC patches the transmitted frame from its TX parameter RAM (duration field, and bytes of the frame); that RAM powers up with random contents, so frames were corrupted depending on the board's history (DHCP packets arrived with a zeroed destination address and ports) | the block is zeroed before the register replay at every TX start |
| W2 | MEDIUM | components/drivers/rf/wifi_mac.c key install | setting IDF's rx-queue policy bit (0x600330D8 bit 1) with the key table stopped the hardware ACK: the peer repeated every frame about 15 times | bit left clear, documented in WIFI_MAC.md |
| W3 | MEDIUM | components/drivers/rf/wifi_mac.c wifi_mac_tx | a 4 ms "quiet window" drained and dropped received frames (including the only ACKed copy of a frame to us and the AP's beacons), producing lost datagrams and false beacon timeouts | replaced by a 400 us guard so that a TX start does not cut off the hardware ACK of the frame just received |
| W4 | MEDIUM | components/wifi/ap.c | an access point that allocates a station at authentication lets 20 junk authentications per second pin every slot | authentication is stateless (8-entry recent list), the slot is taken at association, half-open stations expire after 5 s |
| W5 | LOW | components/tls/tls.c | a peer record larger than the buffer (the default 16 KB records of OpenSSL / browsers without max_fragment_length) | fails closed with a record_overflow alert; tested against OpenSSL |
| W6 | LOW | components/net/http.c | the request head limit and the connection pool are fixed; a handshake that stalls holds the single TLS slot | idle polls free it after 10 s, `https_unit` checks the server recovers after refused handshakes |
| W7 | INFO | components/tls (design) | no certificate validity check (no RTC), no revocation, one connection at a time | accepted, documented |

