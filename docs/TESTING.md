# Testing esp-ndf

Every test is its own application: a folder with its sources and a three-line `Makefile`, built and run
on its own, as in ESP-IDF's `components/<x>/test_apps`.

```
components/drivers/spi/test_apps/spi_unit/
    Makefile          NDF_PATH ?= ../../../../..   +   TEST_* keys   +   include $(NDF_PATH)/make/project.mk
    test_spi.c        void test_spi(void) {...}   NDF_UNIT(010, spi);
    test_spi_hd.c
    ndf_config.h      (optional) options for this test only
    app.mk            (optional) build options (menuconfig writes both)
```

## Running

| What | Command |
|---|---|
| one test application | `cd components/drivers/spi/test_apps/spi_unit && make flash monitor` |
| and judge it | `make test` (in that folder) |
| every unit test of a component | `python3 tools/ndf_test.py run components/drivers/spi` |
| every unit test on the DEVKIT | `make test BOARD=devkit` (top level) |
| list them | `make list` |
| build everything | `make build-all` |
| host fuzzers + signature check | `make security-test` |

`tools/ndf_test.py` builds, flashes, reads the console and reports PASS / FAIL / TIMEOUT per application; the console of a
failing run is kept in `build/testlogs/`. It uses the boards in `make/boards.mk` and stops when the board's USB port
is missing (never retries a board that dropped off the bus: recover it over JTAG first, see `tools/gdb.sh`).

## Keys in an application's Makefile (before the include)

| Key | Meaning | Default |
|---|---|---|
| `UNIT_MAIN = 1` | link the generic runner `make/unit_main.c`: runs every registered test, prints `=== unit: P/N passed ===` | app has its own `main()` |
| `TEST_KIND` | `unit` self-checking, run by default / `bench` prints numbers / `manual` needs a scope, a second board or a host script | `example` (not run) |
| `TEST_BOARD` | `devkit`, `box3` | `devkit` |
| `TEST_PASS` | regex that ends the run and means pass | `=== unit: (\d+)/(\d+) passed ===` with both numbers equal |
| `TEST_TIMEOUT` | seconds to wait | 60 |
| `TEST_HOST` | host script to run (manual tests) | none |
| `TEST_NOTE` | wiring / setup the test needs | none |
| `EXTRA_INCLUDES` | extra include directories (private driver headers) | none |

## Writing a test

```c
#include "test.h"                       // CHECK(cond), METRIC(name, fmt, ...), WAIT_FOR(cond, ms), CYCLES(stmt)
void test_foo(void) { CHECK(foo_init() == 0); METRIC("foo cycles", "%u", CYCLES(foo_run())); }
NDF_UNIT(010, foo);                     // order inside the application (010, 020, ...)
```

Tests that use `unit.h` (`TEST(name) { UASSERT(...); }`) register themselves; an application with its own `main()` calls
`unit_run(NULL)`.

## Where things are

| Folder | Holds |
|---|---|
| `components/<c>/test_apps/` | tests of that component (`arch`, `soc`, `lib`, `drivers/<driver>`) |
| `tests/` | tests that span components: DMA + interrupt stress, the 24 h soak, runtime pieces |
| `examples/` | demonstrations to read and copy, grouped by area; none of them is a test |
| `bench/` | the ESP-IDF / esp-hal siblings and the data behind docs/PERFORMANCE.md |

Wiring for the DEVKIT tests: [WIRING.md](WIRING.md). Security tests: [../SECURITY.md](../SECURITY.md).
