# esp-ndf build.   make [APP=examples/bench] [flash] [monitor]  [PORT=$(BOX3)]
APP  ?= examples/bench
SOC  ?= esp32s3
ARCH ?= xtensa
# Boards by USB id (contains the MAC, stable across replugs).
DEVKIT = /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:D0-if00
BOX3   = /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00
PORT  ?= $(DEVKIT)
TC   ?= $(HOME)/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20250707/xtensa-esp-elf
PFX   = $(TC)/bin/xtensa-$(SOC)-elf-
include soc/$(SOC)/soc.mk

# ---- build-side configuration (override in $(APP)/app.mk or on the command line)
-include $(APP)/app.mk
FLASH_MODE      ?= dio        # image header: mode the ROM loader uses (the driver switches to QIO itself)
FLASH_FREQ      ?= 80m        # image header: SPI clock for the ROM loader
STACK0_SIZE     ?= 16384      # core 0 stack, bytes
STACK1_SIZE     ?= 16384      # core 1 stack, bytes
PART_TABLE_ADDR ?= 0x80000    # partition table offset; the image must end below it
CFG_FLAGS = -DNDF_PART_TABLE_ADDR=$(strip $(PART_TABLE_ADDR)) $(if $(wildcard $(APP)/ndf_config.h),-include $(APP)/ndf_config.h)
ifeq ($(NO_ACCEL),1)
  ACCEL_FLAGS = -DNDF_NO_ACCEL       # every accelerated path off: tests the portable fallbacks
  SOC_LOOPFIX := 0
  OUT   = build/$(notdir $(APP))-noaccel
else
  OUT   = build/$(notdir $(APP))
endif

SRCS  = $(wildcard arch/$(ARCH)/*.c arch/$(ARCH)/*.S soc/$(SOC)/*.c drivers/*/*.c drivers/*/*.S lib/*.c lib/*.S lib/musl/*.c $(APP)/*.c $(APP)/*.S)
OBJS  = $(patsubst %,$(OUT)/%.o,$(SRCS))
INCS  = -Iinclude -Iarch/$(ARCH) -Isoc/$(SOC) -Ibench -Ilib/musl

# CALL0 ABI: no register windows -> cheap, deterministic interrupt entry.
FLAGS = -mabi=call0 -mtext-section-literals -O2 -g -ffreestanding -fno-builtin \
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-function $(ACCEL_FLAGS) $(CFG_FLAGS) $(INCS)

# Rebuild everything when the effective configuration changes (app.mk, command
# line, adding/removing <app>/ndf_config.h); its contents are tracked by .d files.
CFG_SIG := $(FLAGS) $(STACK0_SIZE) $(STACK1_SIZE) $(FLASH_MODE) $(FLASH_FREQ)
$(shell mkdir -p $(OUT); echo '$(CFG_SIG)' | cmp -s - $(OUT)/config.stamp || echo '$(CFG_SIG)' > $(OUT)/config.stamp)
.DEFAULT_GOAL := $(OUT)/app.bin
$(OBJS) $(OUT)/app.elf $(OUT)/app.bin: $(OUT)/config.stamp $(wildcard $(APP)/ndf_config.h)

$(OUT)/app.bin: $(OUT)/app.elf
	esptool.py --chip $(SOC) elf2image --flash_mode $(strip $(FLASH_MODE)) --flash_freq $(strip $(FLASH_FREQ)) -o $@ $< >/dev/null
	@python3 tools/mkunwind.py $(PFX)readelf $(PFX)nm $< $@ >/dev/null   # exact panic backtraces: unwind table in flash
	@test $$(stat -c%s $@) -le $$(( $(strip $(PART_TABLE_ADDR)) )) || (echo "image larger than PART_TABLE_ADDR ($(strip $(PART_TABLE_ADDR))): would overwrite the partition table"; rm -f $@; false)
	@echo "built $@"

$(OUT)/app.elf: $(OBJS) soc/$(SOC)/link.ld
	@$(PFX)gcc $(FLAGS) -nostdlib -T soc/$(SOC)/link.ld -Wl,--gc-sections \
	    -Wl,--defsym=NDF_STACK0_SIZE=$(strip $(STACK0_SIZE)) -Wl,--defsym=NDF_STACK1_SIZE=$(strip $(STACK1_SIZE)) -o $@ $(OBJS)
	$(PFX)size $@

# ESP32-S3 core quirk (measured): re-entering the same zero-overhead loop
# while LBEG still points at it runs the loop at reduced speed (vector copy:
# 2x slower, C loop: ~5%). Every C loop gets "wsr <count reg>, lbeg" before
# its LOOP instruction so LBEG always changes; LOOP then sets it properly.
ifeq ($(strip $(SOC_LOOPFIX)),1)
LOOPFIX = sed -E 's/^([[:space:]]+)(loop|loopnez|loopgtz)([[:space:]]+)(a[0-9]+),/\1wsr\t\4, lbeg\n\1\2\3\4,/'
else
LOOPFIX = cat
endif

# imported musl math (lib/musl/README.md): its own conventions, no warnings
$(OUT)/lib/musl/%.c.o: FLAGS += -include lib/musl/features.h -w

$(OUT)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(PFX)gcc $(FLAGS) -MMD -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)gcc $(FLAGS) -c $(@:.o=.s) -o $@

$(OUT)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS $<"
	@$(PFX)gcc $(FLAGS) -MMD -c $< -o $@

flash: $(OUT)/app.bin
	esptool.py --chip $(SOC) -p $(PORT) write_flash 0x0 $<

PARTS ?= tools/partitions.csv
partition:
	python3 tools/mkpart.py $(PARTS) build/partitions.bin
	esptool.py --chip $(SOC) -p $(PORT) write_flash $(strip $(PART_TABLE_ADDR)) build/partitions.bin

# decode a panic backtrace: make backtrace APP=examples/x ADDR="0x4037.. 0x4037.."
backtrace:
	@$(PFX)addr2line -pfiaC -e $(OUT)/app.elf $(ADDR)

menuconfig:
	python3 tools/ndfconfig/ndfconfig.py $(APP)

config-doc:
	python3 tools/ndfconfig/ndfconfig.py --doc docs/CONFIG.md

monitor:
	python3 -m serial.tools.miniterm $(PORT) 115200

clean:
	rm -rf build

-include $(OBJS:.o=.d)
.PHONY: flash monitor clean partition backtrace
