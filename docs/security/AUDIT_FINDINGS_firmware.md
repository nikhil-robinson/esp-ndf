# Firmware audit findings (esp-ndf, 2026-10-25) - code level, ALL FIXED (status at the end)

Tools: GCC 13 -fanalyzer, cppcheck 2.17, clang 19 analyzer, clang-tidy (cert/bugprone), semgrep (0 hits), flawfinder,
host ASan/UBSan fuzzing (tools/fuzz/fuzz_fat.c, tools/fuzz/fuzz_flashfs.c, tools/test_pfs.c), manual review of every
trust boundary in AUDIT_MAP.md.

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| C1 | CRITICAL | drivers/usb/usb_msc.c:108 | READ/WRITE(10) range check `lba + cnt > blocks()` can wrap | `lba > blocks() \|\| cnt > blocks() - lba`; bounds checks in every bdev |
| C2 | CRITICAL | drivers/storage/fat.c:212-220 | LFN sequence number 0 gives a negative index into a stack buffer (fuzz + clang confirmed) | reject ord < 1 or > 20 |
| H1 | HIGH | drivers/storage/fat.c:283-299, 439, 760 | resolve_walk leaves outputs unset when a parent dir is missing; open(CREAT)/rename use them | initialise outputs, distinct error, callers check |
| H2 | HIGH | drivers/storage/fat.c:626 | fat_readdir copies a name of up to 255 B into a caller buffer of unknown size | add a size argument (fat/spiffs/pfs/vfs readdir) |
| H3 | HIGH | lib/heap.c:107-120 | free() does not validate the pointer / in-use flag (double free, stray pointer) | header magic + used check, panic on mismatch |
| H4 | HIGH | drivers/flash/flash.c:426,573,580; drivers/storage/partition.c:29-31 | `addr + len` range checks can wrap; partition entries not validated against flash size or the image area | overflow-safe checks; reject bad entries |
| H5 | HIGH | drivers/storage/ota.c | OTA image has an integrity hash, no signature: any writer can install firmware | ECDSA P-256 signature check before switching slots |
| H6 | HIGH | Makefile | no stack canaries (-fstack-protector-strong) | add flag + __stack_chk_fail -> panic |
| H7 | HIGH | soc/esp32s3/sys.c | no watchdog armed at boot; panic default halts | default watchdog, documented safe-state policy |
| M1 | MEDIUM | lib/heap.c | block headers without canary: overflows corrupt metadata silently | canary checked on free/alloc |
| M2 | MEDIUM | drivers/storage/partition.c | MD5 entry optional; table accepted without it | require MD5 (configurable) |
| M3 | MEDIUM | soc/esp32s3/coredump.c | 8 KB of raw stack written to flash in plaintext (secrets) | option to omit the stack / clear key buffers |
| M4 | MEDIUM | lib/nn.c:17, 303 | model parameters can cause divide by zero / shift UB (panic) | validate model parameters at load |
| M5 | MEDIUM | system | JTAG + console always open (no production lockdown; eFuse work excluded by owner) | accepted risk, documented; app-level console lock option |
| L1 | LOW | lib/ringbuf.c:12 | buffer alignment not enforced (alignment exception) | require/align to 4 |
| L2 | LOW | drivers/adc/adc.c:130 | analyzer: possible uninitialised read | initialise d[] |
| L3 | LOW | drivers/flash/flash.c:236,252 | SFDP shift UB on bad chip data | range check |
| L4 | LOW | lib/bigint.c:93; soc/esp32s3/rtcclk.c:31 | negative shift / unchecked enum index | argument checks |
| L5 | LOW | soc/esp32s3/system.c:65 | printf format/type mismatch | fix formats |

False positives checked and dismissed: comparePointers on linker-section loops (cmd.c, unit.c, sys.c), ota.c:56
uninit, nvs.c:406 and heap reg[] bounds, 213 "use memcpy_s" advisories, -Wcast-align sites
(all guarded by alignment tests except those listed).

## Status (pass 2, 2026-10-25)
Every row above is fixed (commits 4cd296e, 7c51a3f, be47436 and later); M5 is the accepted eFuse risk (THREAT_MODEL.md).
Re-scan after the fixes (cppcheck --enable=warning, clang analyzer, semgrep p/c) found and fixed:

| ID | Sev | Location | Issue | Fix |
|---|---|---|---|---|
| P1 | MEDIUM | drivers/lcd/i80.c lcd_rgb_start | a zero-byte frame gave k = 0 and wrote fbd[-1] (pass 1 wrongly dismissed it) | `k <= 0` refused |
| P2 | LOW | drivers/flash/flash.c flash_info / oct_init | density codes 0x20/0x21 shifted by 32/33 (L3 fix incomplete); octal path unchecked | 0x20/0x21 mapped to 64/128 MB, everything else bounded |
| P3 | LOW | lib/double.c __fixdfsi | negating INT32_MIN in signed arithmetic | negation done unsigned |
| P4 | MEDIUM | lib/dsp_kalman.c | caller-set n/m larger than DSP_KF_MAX overflowed stack matrices | refused (predict returns, update -1) |
| P5 | MEDIUM | soc/esp32s3/sys.c ndf_panic | default halt-on-panic spun with interrupts masked, so the new interrupt watchdog reset the chip: a boot-time fault became a reboot loop | halt path stops the timer-group watchdogs; examples/panic_halt_test |
| P6 | MEDIUM | soc/esp32s3/sys.c | no way to put actuators in a safe state on a fault | weak ndf_on_panic() hook, first thing in the panic path; crash_test checks it |

Also fixed in pass 2: GDMA channel exhaustion (crypto DMA allocated per operation), M2M peripheral-ID collision with
AES DMA, WL state sanity before recovery (fuzz_wl), printf %n off by default, -Wformat-security, errata CACHE-126 and
LCD-239 workarounds (ERRATA.md). Remaining analyzer reports are reviewed false positives: signed `>> 31`
(implementation-defined, GCC arithmetic shift), `%lu` with uint32_t (unsigned long on Xtensa), loader/ota arrays
filled byte-wise or by slots(), the FIR read-behind window (documented taps-1 history, callers pass it), dead stores.
