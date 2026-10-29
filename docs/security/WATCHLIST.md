# MCU SDK Security & Safety Watchlist - esp-ndf status (2026-10-25)

Every item of the watchlist, with what the SDK does and the evidence. **Fixed** = code changed in this audit;
**Done** = already handled, verified; **Accepted** = owner decision (no eFuse burns), documented in THREAT_MODEL.md;
**N/A** = does not exist on this SDK. Findings and their fixes: AUDIT_FINDINGS_firmware.md, AUDIT_FINDINGS_crypto.md.

## CPU & Memory
| Item | Status | How / evidence |
|---|---|---|
| Stack overflow / corruption | Fixed | -fstack-protector-strong + canary seeded from RNG (sys.c), stack guard window per core and coop task (stackguard.c, coop.c); crash_test step 1 |
| Heap corruption, double free | Fixed | sealed block headers, free/realloc validation, heap_check(), heap_on_corrupt (components/lib/heap.c); runtime_test heap_integrity |
| Buffer overflow / underflow | Fixed | bounded readdir names, FAT LFN index, BLE descriptor spans, ~100 driver index guards, Kalman/NN dimension checks; fuzzers |
| Integer overflow, address wrap | Fixed | overflow-safe range checks (bdev_range_ok, flash in_flash, MSC LBA, OTA/partition parsing, loader seg_ok) |
| Invalid pointer / alignment | Fixed | ringbuf alignment, word copies only on aligned pointers (sha, loader), GDMA alignment checks |
| MPU/MMU configuration, bypass | Fixed | memory protection locked by default (NDF_MEMPROT 2): IRAM not writable, DRAM not executable after boot; runtime_test memprot_locked_refuses_vector_patch |
| Privilege escalation | N/A | one privilege level, one world (bare metal, all code trusted - THREAT_MODEL.md) |
| Unauthorized memory access | Fixed | memprot lock; DMA never reaches loader / panic areas (descriptors built by drivers only); GPIO refuses memory pads |
| Executable RAM / code injection | Fixed | locked memprot: data RAM not executable; vector patching refused (-2) when locked |
| Exception-frame corruption | Fixed | faults switch to a per-core panic stack (vectors.S) - a smashed task stack cannot take the handler down |
| Context-switch register leakage | N/A | cooperative tasks of one trust domain; no isolation boundary to leak across |
| Uninitialized memory disclosure | Fixed | analyzer uninit reports fixed (adc); crypto buffers wiped; core dump omits stacks by default (NDF_COREDUMP_STACK 0) |
| Use-after-free / dangling | Fixed | heap seal cleared on free -> second free / stray pointer caught; fuzzers under ASan |
| Race conditions | Fixed | crypto engine locks (AES/SHA/RSA/ECC), GDMA allocation under lock, RMT rx volatile; stress test |
| Atomicity / memory ordering | Done | atomics with acquire/release on locks, irq_lock around register read-modify-writes |

## Bus, Memory Subsystem & DMA
| Item | Status | How / evidence |
|---|---|---|
| Address decoder aliasing | Done | drivers use one canonical address per region; loader refuses segments outside the image area |
| Out-of-range bus transactions | Fixed | flash / bdev / partition range checks (overflow-safe) |
| Bus master access-control bypass | Accepted | no per-master permission (needs PMS world config + eFuse-backed lock for real effect); DMA buffers come only from drivers |
| DMA access to protected memory | Fixed | crypto DMA only from internal RAM ranges; PSRAM TX DMA refused (silicon returns wrong data - TODO) |
| DMA descriptor corruption | Fixed | descriptor chains bounded (gdma_chain returns -1, callers check), M2M needs an explicit connect |
| DMA buffer lifetime / ownership | Fixed | per-operation channel alloc/free (sha, aes), gdma_free on every path, spi_dma_release |
| Cache coherency / stale data | Fixed | write-back/invalidate on PSRAM DMA start, whole-cache-line rule; CACHE-126 workaround; dma_psram_test |
| MMIO register permissions | Accepted | peripheral PMS not used (one trust domain) |
| Unmapped-access behaviour | Done | load/store faults report through the panic path with PC + backtrace |
| Bus deadlocks / timeouts | Fixed | SPIN_US bound on every hardware wait (clk, adc, sha, aes, spi, i2s, uart, twai, rmt, rf, xcore) |
| SRAM/flash ECC or parity | N/A | ESP32-S3 internal SRAM has no ECC; flash ECC only on some octal parts (off); data integrity by CRC / SHA on every stored structure |
| Isolation between domains | N/A | one domain (bare metal) |

## Interrupts & Exceptions
| Item | Status | How / evidence |
|---|---|---|
| Priority inversion | Done | no blocking locks in ISRs; spinlocks are short and taken with interrupts masked |
| Unbounded nesting | Done | nesting is by level only (max 7 levels, each its own save area - vectors.S) |
| Interrupt storms / DoS | Fixed | interrupt watchdog (TG1, NDF_INT_WDT_MS 1000) resets a core stuck in interrupts; crash_test step 2 |
| ISR/task deadlocks | Fixed | bounded waits; xc_call returns -2 on timeout |
| Vector table modification | Fixed | vectors in locked IRAM; irq_naked refuses (-2) when locked |
| Invalid exception return | Done | single return path per level in vectors.S; faults inside handlers report as faults |
| Fault-handler stack corruption | Fixed | dedicated 2 KB panic stack per core |
| Unhandled exception recovery | Fixed | every vector reports; NDF_PANIC_RESTART restart with crash-loop brake; ndf_on_panic safe-state hook (crash_test) |
| Interrupt state leakage | Done | PS/INTLEVEL restored per frame; FPU state saved per level |

## Peripheral Drivers & HAL
| Item | Status | How / evidence |
|---|---|---|
| Invalid register configurations | Fixed | argument checks before register writes (channel/unit/pin indices, divider ranges, LCD-239 divider) |
| Unchecked input lengths | Fixed | DMA lengths, GT911 points, BLE lengths, USB descriptors clamped |
| Unsafe conversions / truncation | Fixed | shift clamps (nn, bigint, SFDP, flash density), fixdfsi negation |
| Peripheral ownership | Fixed | GDMA channel + M2M peripheral-ID allocation, crypto locks |
| DMA / peripheral races | Fixed | stress test: UART + SPI + GDMA + timer + core 1 for 60 s, 0 errors |
| Timeouts / stuck bus | Fixed | SPIN_US everywhere; TWAI bus-off recovery bounded |
| Reset / clock sequencing | Done | periph_enable clears reset after clock (IDF order) |
| GPIO on boot/reset/security pins | Fixed | flash pads 26-32 never, octal pads 33-37 refused when in use, USB-JTAG 19/20 only with GPIO_FORCE; runtime_test gpio_system_pads_refused |
| Debug UART / diagnostics | Accepted | console + JTAG open (lockdown needs eFuse); the command shell is opt-in per application |
| Untrusted peripheral input | Fixed | parsers fuzzed (FAT, SPIFFS, NVS, WL, MSC, PFS); radio lengths clamped |

## Boot & Firmware Security
| Item | Status | How / evidence |
|---|---|---|
| Secure boot / authenticity | Fixed (app level), Accepted (ROM) | loader checks image header, segment ranges and SHA-256 (NDF_BOOT_VERIFY); OTA images ECDSA-signed. ROM secure boot needs eFuse: accepted |
| Boot ROM trust | Accepted | ROM is immutable silicon; trusted |
| Rollback protection | Fixed | signed version in the OTA trailer, version floor (ota_set_key) |
| Secure updates | Fixed | signature checked before ota_set_boot, only the verified slot can be selected; test_ota_sig: 96/96 bit flips rejected |
| Interrupted-update recovery | Done | A/B slots, otadata with CRC and sequence; loader falls back to a valid slot / factory |
| Bootloader command validation | N/A | loader has no command interface |
| Flash write/erase authorization | Accepted | any code in the image may write flash (one trust domain); partition range checks prevent accidental out-of-partition writes |
| Firmware encryption | Accepted | flash encryption needs eFuse |
| Signing-key protection / rotation | Done | private key never in the image (tools/ota_sign.py keeps it offline); public key + floor set by the app, so rotation = new image with the new key |
| Debug-port lockdown | Accepted | eFuse |
| Provisioning / identity | Accepted | eFuse-based identity not used; MAC from eFuse read-only |
| Factory reset / decommission | Done | factory slot selectable; NVS/filesystem erase APIs; keys in NVS erased with the partition |

## Cryptography & Secrets
| Item | Status | How / evidence |
|---|---|---|
| CSPRNG | Fixed | hardware RNG seeded with SAR ADC noise at boot (rng_boot_seed); runtime_test rng_quality |
| Private-key extraction | Fixed (software), Accepted (physical) | keys wiped after use (explicit_bzero, AES key registers); physical read-out needs flash encryption + JTAG off (eFuse) |
| Hardcoded credentials | Done | semgrep / grep: none; the OTA example key is marked TEST-ONLY |
| Key storage / access control | Accepted | NVS encryption available; key protection at rest needs eFuse HMAC/DS (excluded) |
| Secrets in logs / dumps | Fixed | core dump without stacks by default; printf %n off |
| Sensitive RAM clearing | Fixed | WIPE on every ECC / RSA / GCM path |
| Timing / power side channels | Fixed (timing) | P-256: fixed number of correction passes in the field reduction, every table entry read (fixed-base comb and 4-bit window), branch-free point code; constant-time tag compare. RSA: the engine's fast path shows a FIXED key's bit length and Hamming weight, the same on every call, nothing per message (`NDF_RSA_CONST_TIME 1` removes even that, +34 %); power analysis needs hardware countermeasures (not in SDK) |
| Fault injection on crypto | Fixed | ECDSA: k G recomputed with the negated scalar (different digits and table entries) and s k = e + r d re-checked before a signature leaves (`NDF_ECC_VERIFY_SIGN`, tested by injecting faults: `ecc_unit`); level 2 also verifies. Hedged nonces, glitch + brownout reset on |
| Authentication / replay | Fixed | OTA version floor; GCM tags; replay windows are protocol-level (wireless audit later) |

## RTOS & Concurrency
| Item | Status | How / evidence |
|---|---|---|
| Task privilege separation | N/A | one trust domain |
| Task stack sizing / overflow | Fixed | coop stack guard with margin, canaries |
| Priority inversion | Done | cooperative scheduler: no preemption between tasks |
| Deadlocks / starvation | Fixed | bounded waits, watchdogs |
| Mutex misuse | Fixed | reentrant crypto locks; locks never held across blocking calls |
| Shared-buffer ownership | Fixed | DMA buffers owned by the driver until *_done |
| ISR-to-task communication | Done | lock-free ring buffers, volatile flags |
| Driver API races | Fixed | locks on shared engines; stress test |
| Resource exhaustion / DoS | Fixed | GDMA channels per operation, IRQ share pool 48, heap bounds |
| Watchdog config / recovery | Fixed | interrupt WDT on by default, MWDT/RWDT APIs, crash-loop brake |

## SDK Build System & Toolchain
| Item | Status | How / evidence |
|---|---|---|
| Compiler / linker bugs | Done | pinned Espressif GCC 13 toolchain |
| Unsafe flags | Fixed | -fstack-protector-strong, -Wformat -Wformat-security; no -fno-strict-overflow reliance |
| ABI / calling convention | Done | one ABI (CALL0) for all code, no ROM calls |
| Linker-section overlap | Fixed | link.ld ASSERTs: data end below the loader (0x3FCE7E00), loader IRAM range |
| Startup initialization | Done | bss/rtc-bss zeroed, canary + RNG before main, constructors in order |
| Reproducible builds | Done | two clean builds of periph_test give identical app.bin / ota.bin hashes |
| Dependency / supply chain | Done | no third-party code fetched at build; musl libm sources vendored |
| Third-party library CVEs | Done | only musl libm math (no parsers) |
| Signing keys in CI | Done | no CI holds keys; ota_sign.py takes the key file from the operator |
| Register-header correctness | Done | addresses taken from IDF / TRM, each driver checked on hardware against an IDF sibling |
| Production binary integrity | Fixed | SHA-256 in the image + loader verification, signed OTA |

## Functional Safety & Reliability
| Item | Status | How / evidence |
|---|---|---|
| Brownout / power loss | Done | brownout reset from boot (NDF_BROWNOUT 7); PFS power-cut fuzz: 20000 cuts, 0 torn |
| Clock failure detection | Opt-in | the S3 has no main-crystal fail detector (only for the 32 kHz crystal); rtc_wdt_start() runs on the independent RC slow clock and resets when the main clock dies. Not armed by default: the USB guard and sleep code own the RTC watchdog |
| Watchdog failure / reset loops | Fixed | crash-loop brake (3 early crashes -> halt); halt-on-panic stops the watchdogs so it really halts (panic_halt_test) |
| Safe state after faults | Fixed | ndf_on_panic hook (crash_test); hardware watchdog reset releases every pad |
| Peripheral reset state | Done | drivers reset the peripheral before configuring (e.g. gdma_reset in i2s_start) |
| ECC/parity error response | N/A | no SRAM ECC on this chip (see above) |
| Fault detection coverage | Done | canary, heap seal, memprot, watchdogs, panic on every exception |
| Deadlock detection / recovery | Fixed | watchdogs + bounded waits |
| Flash endurance / wear | Done | WL layer, PFS lazy erase; 24 h soak on BOX-3, 0 fails |
| Thermal / voltage limits | Done | tsens driver, brownout detector |
| Fault logging / post-reset diagnostics | Done | crash record in RTC memory, crash count, reset cause per core, core dump |
| Corrupted configuration recovery | Fixed | CRC / sanity checks on partition table, NVS, WL, otadata, PFS - all fuzzed |

## Testing & Release
| Item | Status | How / evidence |
|---|---|---|
| Static analysis | Done | cppcheck, clang analyzer, clang-tidy, GCC -fanalyzer, semgrep, flawfinder - all reports fixed or reviewed |
| Undefined-behaviour checks | Done | host fuzzers under UBSan; shift/overflow analyzer reports fixed |
| Unit / integration tests | Done | periph_test 41/41, runtime_test 13/13, per-driver examples |
| Fuzzing | Done | make security-test |
| Boundary values | Done | fuzzers + boundary cases in runtime_test / ota signature bit flips |
| DMA / interrupt stress | Done | tests/stress (60 s, 0 errors) |
| Fault injection testing | Done | crash_test (illegal instr, canary, watchdog), PFS power cuts, signature bit flips |
| Hardware-in-the-loop | Done | every test runs on DEVKIT / BOX-3 |
| Silicon errata | Done | ERRATA.md |
| Security regression tests | Done | make security-test + on-target tests (SECURITY.md) |
| Threat model / review | Done | THREAT_MODEL.md, AUDIT_MAP.md |
| Disclosure / patch process | Done | SECURITY.md |

## Radio and network (BLE host, Wi-Fi MAC, station, access point, lwIP, HTTP, TLS) - added 2026-10-29
| Item | Status | How / evidence |
|---|---|---|
| Untrusted frame / PDU parsing | Fixed | every length checked against the buffer before use (ATT, GATT client, 802.11 frames and IEs, EAPOL-Key, DHCP options, X.509 DER, TLS records and handshake messages); fuzzed under ASan/UBSan in `make security-test` (`fuzz_att`, `fuzz_gattc`, `fuzz_wifi_frames`, `fuzz_eapol`, `fuzz_ap`, `fuzz_tls`) |
| Replay of protected traffic | Done | CCMP packet numbers checked per TID and per key (station and AP), EAPOL replay counter, forged / replayed EAPOL messages ignored (`tools/wl/test_sta_wpa.py`) |
| Key handling | Done | PMK / PTK / GTK / TLS secrets wiped with volatile writes when the link goes down or the connection ends; hardware key-table slots cleared per station; the PTK is installed only after the MIC of message 2 / 3 verified |
| Wrong passphrase / bad certificate | Done | station join returns -3 in under 3.5 s; TLS client refuses a wrong CA, a wrong host name and a client without any trust anchor (`tools/wl/test_https.py`, `fuzz_tls`) |
| Denial of service on the access point | Done | stateless open-system authentication (an 8-entry list of recent askers, oldest evicted), stations are allocated at association only, half-open stations freed after 5 s, idle ones after 300 s, `NDF_WIFI_MAX_STA` honoured (status 17); 100 junk authentications at 20/s keep no slot (`tools/wl/test_ap.py`) |
| Unprotected frames on a protected network | Done | the station and the AP accept only EAPOL in the clear on a WPA2 network; unkeyed protected frames are dropped by the MAC |
| Uninitialised hardware state | Fixed | the MAC patches transmitted frames from its parameter RAM, which powers up random: zeroed at every TX start (docs/WIFI_MAC.md); found by sniffing a corrupted DHCP frame |
| Resource exhaustion in the IP stack | Done | fixed lwIP pools (`NDF_NET_*`), pbuf pool exhaustion counted and frames dropped, HTTP/HTTPS connection pools fixed, request head limited to 512 B, one TLS connection at a time; heap back at baseline after malformed requests (`http_unit`) |
| Certificate validity dates | Accepted | the SDK has no real-time clock source: validity is not checked, trust is the pinned CA or leaf key (documented in `components/tls/include/tls.h` and docs/WIRELESS.md) |
| Static group key, no power-save buffering, no WPA3 | Accepted | access point v1 limits; the group key is random per start and not rotated |
| Test keys in the repository | Accepted | `tools/wl/pki`, `components/*/test_apps/*/certs.h`, `bench/idf_wl_peer/main/certs` are lab keys for the interoperability tests; never use them outside the lab |
| Fault injection on signatures | Done | `p256_sign` verifies its own signature before returning (existing hardening), used by the TLS server |

