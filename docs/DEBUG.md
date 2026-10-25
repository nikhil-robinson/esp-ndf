# Debugging esp-ndf apps

## On-chip, no host tools

- **Faults** print the cause, PC and a DWARF-exact backtrace (unwind table in flash, `tools/mkunwind.py`);
  decode with `xtensa-esp32s3-elf-addr2line -pfe build/<app>/app.elf <addresses>`.
- **Watchpoints / perf counters / stack guard**: `debug.h` (`watch_set`, `perf_*`, `stack_guard`). The SP
  monitor runs from boot; cooperative tasks (`coop.h`) move its window on every switch.
- **Logs**: `log.h` levels/tags/timestamps, non-blocking console (`console_try_write` drops instead of waiting).
- **Command line**: `cmd.h` (`CMD(name, help, fn)`), host check `tools/repl_test.py`.

## gdb over the built-in USB-JTAG

```
tools/gdb.sh tests/runtime_test/build/app.elf 24:58:7C:E2:C9:D0 -ex "thbreak unit_run" -ex c
```

`tools/gdb.sh` starts IDF's OpenOCD with two settings esp-ndf needs:

| setting | why |
|---|---|
| `set ESP_FLASH_SIZE 0` | OpenOCD otherwise runs its flasher stub at gdb attach; with our app running it times out and the connection is refused (measured) |
| `set ESP_RTOS none` | no FreeRTOS: plain cores |
| `adapter serial <MAC>` | pick the board when several USB-JTAG units are attached |

Verified on the DEVKIT (2026-10-25): hardware breakpoint at `unit_run`, backtrace `unit_run <- main`, registers
(pc, a1), and structures from the registries (`p *__test_start` prints the first TEST entry). CALL0 code needs no
special gdb settings. Breakpoints in RAM code are hardware ones (2 per core); in flash (XIP) code too.

Flashing stays with `make flash` / esptool (the OpenOCD flash path is off on purpose).

## Not covered yet

- App trace (SystemView-style streaming over JTAG): not implemented; the console log is the fast path today.
- Core dump to flash: open (TODO section 3).
