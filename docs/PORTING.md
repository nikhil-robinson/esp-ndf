# Porting esp-ndf

esp-ndf is layered so that a new chip or CPU replaces one directory, not the drivers.

| layer | directory | contents | changes for |
|---|---|---|---|
| CPU | `arch/<arch>/` | reset entry (`start.S`), vectors + interrupt entry/exit, lazy FPU/PIE save, `irq.c` (interrupt matrix glue), unwinder, `coop_switch.S`, `xtensa.h` (ccount, core id, irq_lock) | another CPU family (e.g. RISC-V for C3/C6/H2) |
| chip | `soc/<chip>/` | link script, clocks (`clk.c`), boot path (`sys.c`), cache/MMU, eFuse table, memory protection, stack guard, RTC/brownout/sleep, `soc_caps.h`, `soc.mk` | another chip |
| peripherals | `drivers/*/` | register-level drivers (UART, SPI, I2S, RMT, ...) | registers moved / fields changed |
| libraries | `lib/` | libc, float/double runtime, DSP, NN, crypto math, time, events, REPL, tests, coop tasks | nothing (portable C; accelerated paths behind capability flags) |

Build selection: `make SOC=<chip> ARCH=<arch>` (`Makefile` picks `arch/$(ARCH)` and `soc/$(SOC)`).

## Capability flags (`soc/<chip>/soc_caps.h`)

Every accelerated path is chosen by a flag and has a C fallback: `SOC_CPU_HAS_FPU`, `SOC_CPU_HAS_PIE` (128-bit
vectors), `SOC_CPU_HAS_S32C1I` (compare-and-swap), `SOC_HAS_FLASH_CACHE`, `SOC_CPU_HWLOOP_LBEG_QUIRK`,
`SOC_CPU_CORES`, `SOC_HAS_CROSS_CORE_INT`. `make NO_ACCEL=1` builds this chip with every fallback forced on: that is
how the portable paths are tested here (examples/dsp_lib, nn_lib, libc_test, fastmath_test pass both ways).

## A new ESP chip with the same CPU (e.g. ESP32-S2)

1. `soc/<chip>/link.ld.in`: memory map (IRAM/DRAM aliases, RTC memories, flash/PSRAM windows).
2. `soc_caps.h`, `soc.mk`: features; `soc.h`: core count, XTAL.
3. `clk.c`: PLL/CPU clock switch, peripheral clock + reset bits (`periph_enable` table).
4. `sys.c`: boot path (cache bring-up, stacks, core 1 start), `cache.c`, `efuse_table.h` (generate with
   `tools/gen_efuse.py` from IDF's esp_efuse_table.csv).
5. Drivers: same IP blocks, other base addresses or interrupt sources. Each driver keeps its addresses and
   signal numbers as defines at the top of its file; read IDF's `hal/<chip>/include/hal/*_ll.h` and `soc/<chip>/`
   headers for the new values (rule: read the reference first). Re-run `examples/periph_test` on the new board.

## A new CPU family

Implement in `arch/<arch>/`: entry, vector table / interrupt entry that saves the caller-saved registers, the
`irq_*` API (`irq.h`), `irq_lock/unlock`, a cycle counter for `ccount()`, `coop_switch` for the callee-saved set
of that ABI, and the fault handler + unwinder. Everything above (drivers, lib) stays.

## What is S3-only today

The register maps in `drivers/` are the ESP32-S3's; DSP/NN assembly is Xtensa + PIE (with C fallbacks); the RF
drivers (`drivers/rf`) are S3 radio specific. No second chip has been brought up yet, so this guide is the plan
the layering was built for, checked only by the NO_ACCEL build.
