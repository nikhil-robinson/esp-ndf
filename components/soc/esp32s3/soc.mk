# ESP32-S3 build capabilities (make side of soc_caps.h)
ARCH ?= xtensa
TC   ?= $(HOME)/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20250707/xtensa-esp-elf
PFX   = $(TC)/bin/xtensa-$(SOC)-elf-
# CALL0 ABI: no register windows -> cheap, deterministic interrupt entry.
ARCH_FLAGS = -mabi=call0 -mlongcalls -mtext-section-literals
SOC_DRIVER_SRCS = $(wildcard $(C)/drivers/*/*.c $(C)/drivers/*/*.S)
SOC_LIB_EXCLUDE =
FLASH_APP_ADDR = 0x0
SOC_LOOPFIX := 1        # re-entered hardware loops need LBEG perturbed (see Makefile)
