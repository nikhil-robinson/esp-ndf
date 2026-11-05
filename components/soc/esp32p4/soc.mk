# ESP32-P4 (v3.x silicon) build capabilities (make side of soc_caps.h). Reference: ESP-IDF v6.1.
ARCH ?= riscv
TC   ?= $(HOME)/.espressif-v6.1/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf
PFX   = $(TC)/bin/riscv32-esp-elf-
# No Zcmp: IDF SOC_CPU_ZCMP_WORKAROUND (push/pop and interrupts, DIG-661). PIE / hardware loops only in their own .S files.
ARCH_FLAGS = -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mno-relax -Wl,--no-warn-rwx-segments
# P4 drivers live in drivers/<x>/esp32p4/ (same public headers); folders without one are not built for this chip
# plus the drivers shared with the S3 (same file, chip data from soc_caps.h): storage on top of flash.h (ota.c waits for the
# P4 boot path) and the crypto engines
SOC_DRIVER_SRCS = $(wildcard $(C)/drivers/*/esp32p4/*.c $(C)/drivers/*/esp32p4/*.S) $(filter-out $(C)/drivers/storage/ota.c,$(wildcard $(C)/drivers/storage/*.c)) $(wildcard $(C)/drivers/crypto/*.c) $(C)/soc/esp32s3/time.c $(C)/soc/esp32s3/efuse.c $(C)/drivers/pcnt/pcnt.c $(C)/drivers/mcpwm/mcpwm.c $(C)/drivers/rmt/rmt.c $(C)/drivers/i2c/i2c.c $(C)/drivers/spi/spi.c $(C)/drivers/xcore/xcore.c $(C)/drivers/sdm/sdm.c $(C)/drivers/twai/twai.c $(C)/drivers/lcd/lcd.c $(C)/drivers/lcd/gt911.c $(C)/drivers/lcd/i80.c $(C)/drivers/lcd/cam.c
# Xtensa assembly in components/lib; the C paths cover it
SOC_LIB_EXCLUDE = double.S dsp.S dsp_f32.S fpu.S nn_s8.S
# the P4 ROM reads the first image at 0x2000
FLASH_APP_ADDR = 0x2000
# the ROM loads the app directly (no own loader yet)
BOOT_FAST ?= 0
XIP ?= off
SOC_LOOPFIX := 0
