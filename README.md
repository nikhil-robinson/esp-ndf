# esp-ndf — Bare-Metal ESP32-S3 SDK

A minimal, dependency-free bare-metal SDK for the **ESP32-S3** (Xtensa LX7).
No ESP-IDF runtime, no FreeRTOS, no HAL abstraction tower — just your code,
a linker script, a startup stub, and the hardware.

## Repository Layout

```
esp-ndf/
├── arch/
│   └── xtensa/
│       ├── include/
│       │   ├── xtensa_ops.h    # RSR/WSR macros, interrupt enable/disable
│       │   └── intr.h          # Peripheral interrupt routing API
│       ├── ld/
│       │   ├── esp32s3.ld      # Full linker script (IRAM/DRAM/IROM/DROM)
│       │   └── esp32s3.rom.ld  # ROM function symbol stubs
│       ├── startup/
│       │   ├── vectors.S       # 17 Xtensa LX7 interrupt/exception vectors
│       │   └── start.S         # CPU0 entry: WDT disable → VECBASE → main
│       ├── intr.c              # Interrupt matrix routing
│       └── CMakeLists.txt
├── board/
│   └── esp32s3/
│       ├── include/
│       │   ├── board.h         # Top-level board constants and API
│       │   └── soc/
│       │       ├── soc.h           # Peripheral base addresses + REG_READ/WRITE
│       │       ├── gpio_reg.h      # GPIO controller registers
│       │       ├── uart_reg.h      # UART controller registers
│       │       ├── system_reg.h    # CPU frequency + peripheral clock gates
│       │       ├── rtc_cntl_reg.h  # RTC controller + WDT registers
│       │       └── io_mux_reg.h    # IO MUX pad configuration
│       ├── board_init.c        # board_init(): WDTs, PLL→240MHz, UART0
│       └── CMakeLists.txt
├── cmake/
│   ├── toolchain-xtensa-esp32s3.cmake  # Cross-compilation toolchain file
│   ├── esp32s3.cmake                   # Flash geometry, app offset, cflags
│   └── utils.cmake                     # ndf_add_executable / ndf_add_library
├── drivers/
│   ├── gpio/
│   │   ├── gpio.h              # GPIO direction, level, ISR API
│   │   └── gpio.c
│   ├── uart/
│   │   ├── uart.h              # UART init, putc/getc, uart_printf
│   │   └── uart.c
│   └── CMakeLists.txt
├── include/
│   ├── types.h                 # uint8/16/32/64_t, bool, BIT(), etc. (no libc)
│   └── compiler.h              # IRAM_ATTR, ISR_ATTR, PACKED, ALIGNED, …
├── lib/
│   ├── string.h                # memset / memcpy / strlen / strcmp / …
│   ├── string.c
│   └── CMakeLists.txt
├── examples/
│   └── blink/
│       ├── main.c              # Toggle GPIO48 at 1 Hz, print ticks to UART0
│       └── CMakeLists.txt
└── tools/
    ├── flash.py                # Build → esptool elf2image → write_flash
    └── get_toolchain.py        # Download prebuilt xtensa-esp32s3-elf toolchain
```

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| `xtensa-esp32s3-elf-gcc` | ≥ 13.2.0 | Espressif prebuilt, see below |
| CMake | ≥ 3.20 | `cmake --version` |
| Python 3 | ≥ 3.8 | `python --version` |
| esptool.py | ≥ 4.7 | `pip install esptool` |
| pyserial | ≥ 3.5 | `pip install pyserial` (for monitor) |

---

## Step 1 — Download the Toolchain

### Automatic (recommended)

```bash
python tools/get_toolchain.py
```

This downloads and extracts the Espressif prebuilt
`xtensa-esp32s3-elf-13.2.0_20240530` toolchain to `tools/xtensa-esp32s3-elf/`.

#### Add to PATH

**Linux / macOS (bash/zsh):**
```bash
export PATH="$(pwd)/tools/xtensa-esp32s3-elf/bin:$PATH"
```

**Windows PowerShell:**
```powershell
$env:PATH = "$(Get-Location)\tools\xtensa-esp32s3-elf\bin;$env:PATH"
```

### Manual

Download from the [Espressif crosstool-NG releases](https://github.com/espressif/crosstool-NG/releases):

```
xtensa-esp32s3-elf-13.2.0_20240530-<platform>.tar.xz
```

Available platforms: `x86_64-linux-gnu`, `aarch64-apple-darwin`,
`x86_64-apple-darwin`, `x86_64-w64-mingw32`.

Verify after install:
```bash
xtensa-esp32s3-elf-gcc --version
# xtensa-esp32s3-elf-gcc (crosstool-NG 13.2.0_20240530) 13.2.0
```

---

## Step 2 — Build

```bash
# From the repo root:
mkdir build
cd build

# Configure (toolchain file is set by default in the top-level CMakeLists.txt)
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel

# Or point to toolchain explicitly if get_toolchain.py was not used:
# cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-xtensa-esp32s3.cmake

# Build everything (includes blink example)
cmake --build . -- -j$(nproc)
```

**Windows (PowerShell + Ninja):**
```powershell
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
ninja
```

Successful output:
```
[100%] Linking C executable blink.elf
   text    data     bss     dec     hex filename
   4218     108      24    4350    10fe blink.elf
[100%] Built target blink
```

---

## Step 3 — Flash

### Using `tools/flash.py`

```bash
# From repo root:
python tools/flash.py --port /dev/ttyUSB0 build/examples/blink/blink.elf

# Windows:
python tools\flash.py --port COM3 build\examples\blink\blink.elf
```

options:
```
--port     Serial port (default: /dev/ttyUSB0)
--baud     Flash baud rate (default: 460800)
--monitor  Open serial monitor after flashing
--erase-all  Erase entire flash before writing
```

### Manual (esptool directly)

```bash
# Convert ELF → binary
esptool.py --chip esp32s3 elf2image \
    --output blink.bin \
    build/examples/blink/blink.elf

# Flash (partition table + app)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
    write_flash \
    0x8000  partition_table.bin \
    0x10000 blink.bin
```

### Expected UART0 output (115200 baud, GPIO43=TX)

```
ESP32-S3 bare-metal SDK starting...
board_init: clocks configured
board_init: UART0 ready at 115200 baud
tick 0: LED ON
tick 1: LED OFF
tick 2: LED ON
...
```

---

## Memory Map

| Region | Start | Size | Used for |
|--------|-------|------|----------|
| IRAM | `0x40378000` | 288 KB | Vector table, ISRs, critical code (`IRAM_ATTR`) |
| DRAM | `0x3FC88000` | 384 KB | `.data`, `.bss`, heap, stack |
| IROM (flash) | `0x42000020` | 8 MB | `.text`, `.literal` (I-cache mapped) |
| DROM (flash) | `0x3C000020` | 8 MB | `.rodata` (D-cache mapped) |
| RTC IRAM | `0x600FE000` | 8 KB | Deep-sleep-safe code (`RTC_IRAM_ATTR`) |
| RTC Slow | `0x50000000` | 8 KB | Deep-sleep-persistent data |

Stack: 8 KB, top of DRAM (`_stack_top = 0x3FCE8000`).
Heap: everything between `_heap_start` (end of BSS) and `_stack_top - 8K`.

---

## Adding a New Example

1. Create `examples/my_app/main.c`:
```c
#include "board.h"
#include "drivers/uart.h"

int main(void) {
    uart_printf(0, "Hello from my_app!\n");
    for(;;) {}
}
```

2. Create `examples/my_app/CMakeLists.txt`:
```cmake
ndf_add_executable(my_app
    main.c
    LINK_LIBRARIES
        arch_xtensa
        board_esp32s3
        drivers
        ndf_libc
)
```

3. Add to top-level `CMakeLists.txt`:
```cmake
add_subdirectory(examples/my_app)
```

4. Rebuild:
```bash
cmake --build build -- -j$(nproc)
```

---

## Adding a Driver

Place source files under `drivers/<name>/`:

```
drivers/spi/
├── spi.h
├── spi.c
└── (add to drivers/CMakeLists.txt)
```

In `drivers/CMakeLists.txt`, add `spi/spi.c` to the `drivers` library sources.

---

## Interrupt Handling

Register a peripheral ISR:
```c
#include "arch/xtensa/include/intr.h"

static void IRAM_ATTR my_gpio_isr(void *arg) {
    // read GPIO_STATUS, clear it
}

// In board_init or after:
intr_alloc(INTR_SRC_GPIO, 2, my_gpio_isr, NULL, NULL);
intr_enable(2);
```

`INTR_SRC_*` constants are in `arch/xtensa/include/intr.h`.
ISRs **must** be marked `IRAM_ATTR` so they run from RAM when flash is busy.

---

## Architecture

### Startup Sequence

```
ROM bootloader
  └─▶ call_start_cpu0()  [vectors.S + start.S, IRAM]
        ├─ Disable TIMG0/TIMG1/RTC watchdog timers
        ├─ WSR VECBASE ← _vectors_start (0x40378000, 0x400-aligned)
        ├─ WSR PS ← INTLEVEL=15, WOE=1, UM=1
        ├─ Clear windowed register file
        ├─ SP ← _stack_top
        └─▶ __start_c()
              ├─ Copy .data  (LMA flash → VMA DRAM)
              ├─ Zero  .bss
              └─▶ board_init()   → switch_cpu_to_240mhz(), uart_init(), …
                    └─▶ main()
```

### Linker Script Sections

| Section | Memory | Description |
|---------|--------|-------------|
| `.vectors` | IRAM @ `_vectors_start` | 17 Xtensa exception/interrupt vectors, padded to 0x400 bytes |
| `.iram.text` | IRAM | `IRAM_ATTR` functions, `__start_c` |
| `.iram.data` | IRAM | `IRAM_ATTR` globals |
| `.text` + `.literal` | IROM | Application code (I-cache) |
| `.rodata` | DROM | `const` data (D-cache) |
| `.data` | DRAM (LMA=DROM) | Initialised globals (copied at startup) |
| `.bss` | DRAM | Zero-initialised globals |
| `.rtc.text` | RTC IRAM | `RTC_IRAM_ATTR` (deep-sleep safe) |

---

## Porting to Another Architecture (e.g. RISC-V / ESP32-C3)

1. Add `arch/riscv/` with:
   - `startup/start.S` — no windowed ABI, `mret` for interrupt return
   - `ld/esp32c3.ld` — ESP32-C3 memory regions
   - `include/riscv_ops.h` — CSR macros (`csrr`, `csrw`)
   - `intr.c` — PLIC-based interrupt routing (different from Xtensa INTMATRIX)
2. Add `board/esp32c3/` with SoC headers from the C3 TRM.
3. Add `cmake/toolchain-riscv32-esp32c3.cmake` using `riscv32-esp-elf-gcc`.
4. Set `-DARCH=riscv -DBOARD=esp32c3` in CMake configure step.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Device immediately resets | WDT timeout — `call_start_cpu0` not reached | Check flash offset: app must be at `0x10000` |
| Garbage UART output | Wrong baud or APB clock misconfigured | Verify `board_init` ran before `uart_init` |
| Hard fault / LoadStoreError | Function without `IRAM_ATTR` called from ISR | Add `IRAM_ATTR` to ISR and all functions it calls |
| `movi` assembler error on large constant | Use `mowi` macro instead | `mowi reg, symbol` → expands to `l32r` |
| Linker: "relocation truncated" | `.literal` pool not adjacent to `.text` | Use `-mtext-section-literals` (already set in toolchain file) |
| Flash fails: wrong stub | Old esptool.py | `pip install --upgrade esptool` |

---

## License

This project is placed in the **public domain** (or CC0-1.0 where public domain
is not recognised). Use it as a starting point for your own firmware without
any obligation to upstream changes.
