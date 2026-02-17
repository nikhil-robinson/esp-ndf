# ESP32-S3 Bare Metal SDK

Bare metal SDK (no ESP-IDF) for ESP32 series with Xtensa/RISC-V CPU bring-up and peripheral control.

## Project Structure

```
esp-ndf/
├── Makefile                    # Top-level build orchestration
├── tools/
│   └── toolchain.mk            # Toolchain paths & flags
├── arch/
│   ├── esp32/                  # Xtensa LX6 dual-core (TODO)
│   ├── esp32s3/                # Xtensa LX7 dual-core
│   │   ├── esp32s3.h           # Arch definitions, memory map
│   │   └── system.c            # System control
│   └── esp32c3/                # RISC-V single-core (TODO)
├── hal/
│   ├── gpio.c / gpio.h         # GPIO abstraction
│   └── uart.c / uart.h         # UART abstraction
├── core/
│   ├── startup.S               # Reset vector, stack init
│   ├── vectors.S               # Interrupt/exception table
│   ├── interrupt.c / .h        # Interrupt management
├── reg/
│   ├── gpio_reg.h              # Raw GPIO register defines
│   ├── uart_reg.h              # Raw UART register defines
│   └── timer_reg.h             # Raw Timer register defines
├── ld/
│   ├── esp32s3.ld              # Main linker script
│   └── memory.ld               # Memory map
└── examples/
    ├── blink/                  # GPIO blink example
    └── uart_hello/             # UART hello world (TODO)
```

## Requirements

- Xtensa ESP32-S3 toolchain (`xtensa-esp32s3-elf-*`)
- esptool: `pip3 install esptool` (uses `python3 -m esptool`)
- GNU Make

### Install Toolchain

Download from Espressif:
```bash
# Download toolchain
wget https://github.com/espressif/crosstool-NG/releases/download/esp-12.2.0_20230208/xtensa-esp32s3-elf-12.2.0_20230208-x86_64-linux-gnu.tar.xz

# Extract
tar -xf xtensa-esp32s3-elf-12.2.0_20230208-x86_64-linux-gnu.tar.xz

# Add to PATH
export PATH=$PATH:$PWD/xtensa-esp32s3-elf/bin
```

## Build

### Build from SDK root (builds all SDK components):
```bash
make clean
make TARGET=esp32s3
```

### Build example:
```bash
cd examples/blink
make clean
make
```

## Flash

```bash
make flash PORT=/dev/ttyACM0
```

## Supported Targets

| Target   | Architecture | Cores | Status |
|----------|-------------|-------|--------|
| esp32s3  | Xtensa LX7  | 2     | ✅ Active |
| esp32    | Xtensa LX6  | 2     | 🚧 TODO |
| esp32c3  | RISC-V      | 1     | 🚧 TODO |

## Features

- **No ESP-IDF** - Pure bare metal, direct register access
- **HAL Layer** - Clean abstraction for peripherals
- **Multi-target** - Same SDK supports multiple ESP32 variants
- **Minimal startup** - Fast boot, no overhead

## Notes

- This is raw bare metal - no bootloader dependency
- Direct register manipulation from TRM
- Minimal CPU initialization for maximum control
