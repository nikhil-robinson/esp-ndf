# ESP32-P4 (v3.x silicon) build capabilities (make side of soc_caps.h). Reference: ESP-IDF v6.1.
ARCH ?= riscv
# P4_REV=1: silicon v1.x ("eco2" ROM, e.g. the v1.3 boards); default 3 (v3.x)
P4_REV ?= 3
ifeq ($(P4_REV),1)
override DEFS += -DNDF_P4_REV=1
endif
TC   ?= $(HOME)/.espressif-v6.1/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf
PFX   = $(TC)/bin/riscv32-esp-elf-
# No Zcmp: IDF SOC_CPU_ZCMP_WORKAROUND (push/pop and interrupts, DIG-661). PIE / hardware loops only in their own .S files.
ARCH_FLAGS = -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mno-relax -Wl,--no-warn-rwx-segments
# P4 drivers live in drivers/<x>/esp32p4/ (same public headers); folders without one are not built for this chip
# plus the drivers shared with the S3 (same file, chip data from soc_caps.h): storage on top of flash.h (ota.c waits for the
# P4 boot path) and the crypto engines
SOC_DRIVER_SRCS = $(wildcard $(C)/lib/esp32p4/*.c $(C)/lib/esp32p4/*.S) $(wildcard $(C)/drivers/*/esp32p4/*.c $(C)/drivers/*/esp32p4/*.S) $(wildcard $(C)/drivers/storage/*.c) $(wildcard $(C)/drivers/crypto/*.c) $(C)/soc/esp32s3/time.c $(C)/soc/esp32s3/efuse.c $(C)/drivers/pcnt/pcnt.c $(C)/drivers/mcpwm/mcpwm.c $(C)/drivers/rmt/rmt.c $(C)/drivers/i2c/i2c.c $(C)/drivers/spi/spi.c $(C)/drivers/xcore/xcore.c $(C)/drivers/sdm/sdm.c $(C)/drivers/twai/twai.c $(C)/drivers/lcd/lcd.c $(C)/drivers/lcd/gt911.c $(C)/drivers/lcd/i80.c $(C)/drivers/lcd/cam.c $(C)/drivers/usb/usb_dev.c $(C)/drivers/usb/usb_msc.c $(C)/soc/esp32s3/pm.c
# Xtensa assembly in components/lib (and dsp.c, its portable twin) is replaced by lib/esp32p4: PIE kernels + C
SOC_LIB_EXCLUDE = double.S dsp.S dsp_f32.S fpu.S nn_s8.S dsp.c
# the P4 ROM reads the first image at 0x2000
FLASH_APP_ADDR = 0x2000
# BOOT_FAST=0: the ROM loads the app directly. BOOT_FAST=1: the ROM loads loader/loader.c (OTA slots, image check) which
# copies the app from BOOT_LOAD_ADDR; no RTC boot stage on the P4 (every reset goes through the ROM)
BOOT_FAST ?= 0
BOOT_LOAD_ADDR ?= 0x10000
LOADER_SRCS = loader.S loader.c
LOADER_ARCH = $(ARCH_FLAGS)
LOADER_EXTRA =
LOADER_FLASH = 0x2000 $(OUT)/loader.bin
XIP ?= off
SOC_LOOPFIX := 0
