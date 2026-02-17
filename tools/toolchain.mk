# Toolchain Configuration
# Select toolchain based on target architecture

# Supported targets: esp32, esp32s3, esp32c3
TARGET ?= esp32s3

# Toolchain paths (override these or set in environment)
TOOLCHAIN_PATH ?=

ifeq ($(TARGET),esp32)
    # ESP32 - Xtensa LX6 dual-core
    TOOLCHAIN_PREFIX = xtensa-esp32-elf-
    ARCH_FLAGS = -mlongcalls -mtext-section-literals
    ARCH = xtensa
else ifeq ($(TARGET),esp32s3)
    # ESP32-S3 - Xtensa LX7 dual-core
    TOOLCHAIN_PREFIX = xtensa-esp32s3-elf-
    ARCH_FLAGS = -mlongcalls -mtext-section-literals
    ARCH = xtensa
else ifeq ($(TARGET),esp32c3)
    # ESP32-C3 - RISC-V single-core
    TOOLCHAIN_PREFIX = riscv32-esp-elf-
    ARCH_FLAGS = -march=rv32imc -mabi=ilp32
    ARCH = riscv
else
    $(error Unsupported TARGET: $(TARGET). Use esp32, esp32s3, or esp32c3)
endif

# Toolchain binaries
ifneq ($(TOOLCHAIN_PATH),)
    TOOLCHAIN_BIN = $(TOOLCHAIN_PATH)/bin/
else
    TOOLCHAIN_BIN =
endif

CC      = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)gcc
AS      = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)gcc
LD      = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)ld
AR      = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)ar
OBJCOPY = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)objcopy
OBJDUMP = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)objdump
SIZE    = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)size
GDB     = $(TOOLCHAIN_BIN)$(TOOLCHAIN_PREFIX)gdb

# Common compiler flags
CFLAGS_COMMON = -ffunction-sections -fdata-sections
CFLAGS_COMMON += -fno-builtin -nostdlib
CFLAGS_COMMON += -Wall -Wextra
CFLAGS_COMMON += -O2 -g

# Common assembler flags
ASFLAGS_COMMON = -g

# Common linker flags
LDFLAGS_COMMON = -nostdlib --gc-sections

# Combine arch-specific and common flags
CFLAGS  = $(ARCH_FLAGS) $(CFLAGS_COMMON)
ASFLAGS = $(ARCH_FLAGS) $(ASFLAGS_COMMON)
LDFLAGS = $(LDFLAGS_COMMON)

ESPTOOL = python3 -m esptool
PORT ?= /dev/ttyACM0
BAUD ?= 460800
