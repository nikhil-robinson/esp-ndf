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

# ---- build-side configuration (override in $(APP)/app.mk or on the command line; DEFS="-DNDF_X=1" for one-off
# option overrides, e.g. benchmark variants)
-include $(APP)/app.mk
FLASH_MODE      ?= dio        # image header: mode the ROM loader uses (the driver switches to QIO itself)
FLASH_FREQ      ?= 80m        # image header: SPI clock for the ROM loader
STACK0_SIZE     ?= 16384      # core 0 stack, bytes
STACK1_SIZE     ?= 16384      # core 1 stack, bytes
PART_TABLE_ADDR ?= 0x80000    # partition table offset; the image must end below it
BOOT_FAST       ?= 1          # 1: the ROM loads a small loader that copies the app from BOOT_LOAD_ADDR (fast boot)
BOOT_LOAD_ADDR  ?= 0x2000     # flash offset of the app image when BOOT_FAST = 1
XIP             ?= lib        # code from flash: off (all in RAM) | lib (libraries in flash) | all (libraries, app, storage)
XIP_ADDR        ?= 0x300000   # flash offset of the code blob (xip partition), 64 KB aligned
XIP_SIZE        ?= 0x100000   # its size
XIP_NUM = $(if $(filter off,$(strip $(XIP))),0,$(if $(filter all,$(strip $(XIP))),2,1))
LOAD_AT = $(if $(filter 1,$(strip $(BOOT_FAST))),$(strip $(BOOT_LOAD_ADDR)),0)
CFG_FLAGS = $(DEFS) -DNDF_BOOT_LOAD_ADDR=$(LOAD_AT) -DNDF_XIP=$(XIP_NUM) -DNDF_XIP_ADDR=$(strip $(XIP_ADDR)) -DNDF_PART_TABLE_ADDR=$(strip $(PART_TABLE_ADDR)) $(if $(wildcard $(APP)/ndf_config.h),-include $(APP)/ndf_config.h)
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
FLAGS = -mabi=call0 -mlongcalls -mtext-section-literals -O2 -g -ffreestanding -fno-builtin \
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-function $(ACCEL_FLAGS) $(CFG_FLAGS) $(INCS)

# Rebuild everything when the effective configuration changes (app.mk, command
# line, adding/removing <app>/ndf_config.h); its contents are tracked by .d files.
CFG_SIG := $(FLAGS) $(STACK0_SIZE) $(STACK1_SIZE) $(FLASH_MODE) $(FLASH_FREQ) $(XIP) $(XIP_ADDR) $(XIP_SIZE) $(BOOT_FAST) $(BOOT_LOAD_ADDR)
$(shell mkdir -p $(OUT); echo '$(CFG_SIG)' | cmp -s - $(OUT)/config.stamp || echo '$(CFG_SIG)' > $(OUT)/config.stamp)
.DEFAULT_GOAL := all
all: $(OUT)/app.bin $(if $(filter-out 0,$(LOAD_AT)),$(OUT)/loader.bin $(OUT)/rtcboot.bin)
.PHONY: all
$(OBJS) $(OUT)/app.elf $(OUT)/app.bin: $(OUT)/config.stamp $(wildcard $(APP)/ndf_config.h)

$(OUT)/app.bin: $(OUT)/app.elf
	@$(PFX)objcopy -R .flash.text -R .flash.rodata $< $(OUT)/app_ram.elf 2>&1 | grep -v "empty loadable segment" >&2 || true   # the ROM loads only the RAM part
	esptool.py --chip $(SOC) elf2image --flash_mode $(strip $(FLASH_MODE)) --flash_freq $(strip $(FLASH_FREQ)) -o $@ $(OUT)/app_ram.elf >/dev/null
	@$(PFX)objcopy -O binary -j .flash.text $< $(OUT)/xip_text.bin; $(PFX)objcopy -O binary -j .flash.rodata $< $(OUT)/xip_ro.bin
	@cat $(OUT)/xip_text.bin $(OUT)/xip_ro.bin > $(OUT)/xip.bin                  # code blob for XIP_ADDR (rodata right after the code)
	@test $$(stat -c%s $(OUT)/xip.bin) -le $$(( $(strip $(XIP_SIZE)) )) || (echo "xip.bin larger than XIP_SIZE"; rm -f $@; false)
	@python3 tools/mkunwind.py $(PFX)readelf $(PFX)nm $< $@ >/dev/null   # exact panic backtraces: unwind table in flash
	@test $$(( $$(stat -c%s $@) + $(LOAD_AT) )) -le $$(( $(strip $(PART_TABLE_ADDR)) )) || (echo "image larger than PART_TABLE_ADDR ($(strip $(PART_TABLE_ADDR))): would overwrite the partition table"; rm -f $@; false)
	@echo "built $@"

$(OUT)/loader.bin: soc/$(SOC)/loader/loader.S soc/$(SOC)/loader/loader.c soc/$(SOC)/loader/loader.ld soc/$(SOC)/regi2c.c $(OUT)/config.stamp
	@$(PFX)gcc -mabi=call0 -mtext-section-literals -Os -ffreestanding -nostdlib -ffunction-sections -fdata-sections $(INCS) $(CFG_FLAGS) \
	    $(if $(BOOT_TRACE),-DNDF_BOOT_TRACE=1) -Wl,--gc-sections -T soc/$(SOC)/loader/loader.ld -o $(OUT)/loader.elf \
	    soc/$(SOC)/loader/loader.S soc/$(SOC)/loader/loader.c soc/$(SOC)/regi2c.c   # analog-register access shared with the app
	@esptool.py --chip $(SOC) elf2image --flash_mode $(strip $(FLASH_MODE)) --flash_freq $(strip $(FLASH_FREQ)) -o $@ $(OUT)/loader.elf >/dev/null
	@echo "built $@ ($$(stat -c%s $@) bytes; the ROM loads only this)"

# RTC boot stage: the same loader, run from RTC fast memory behind a jump at the CPU reset vector 0x50000000 (RTC slow
# memory, slow to fetch from; loader_rtc.ld). The flash loader copies it
# there on a cold boot and points the reset vector at it: warm resets and deep-sleep wakes then boot without the ROM.
$(OUT)/rtcboot.bin: soc/$(SOC)/loader/loader.S soc/$(SOC)/loader/loader.c soc/$(SOC)/loader/loader_rtc.ld soc/$(SOC)/regi2c.c $(OUT)/config.stamp
	@$(PFX)gcc -mabi=call0 -mtext-section-literals -Os -ffreestanding -nostdlib -ffunction-sections -fdata-sections $(INCS) $(CFG_FLAGS) \
	    -DNDF_LOADER_RTC=1 $(if $(BOOT_TRACE),-DNDF_BOOT_TRACE=1) -Wl,--gc-sections -T soc/$(SOC)/loader/loader_rtc.ld -o $(OUT)/rtcboot.elf \
	    soc/$(SOC)/loader/loader.S soc/$(SOC)/loader/loader.c soc/$(SOC)/regi2c.c
	@test $$($(PFX)size -A $(OUT)/rtcboot.elf | awk '$$1==".data"{d=$$2} END{print d+0}') -eq 0 || (echo "rtcboot: .data must be empty (copied raw)"; false)
	@$(PFX)objcopy -O binary -j .rtc $(OUT)/rtcboot.elf $(OUT)/rtcboot_jmp.raw
	@$(PFX)objcopy -O binary -j .text $(OUT)/rtcboot.elf $(OUT)/rtcboot.raw
	@python3 -c "import struct; j,d=(open('$(OUT)/rtcboot'+n,'rb').read() for n in ('_jmp.raw','.raw')); j+=b'\\0'*(-len(j)%4); d+=b'\\0'*(-len(d)%4); open('$@','wb').write(struct.pack('<III',0x42435452,len(j),len(d))+j+d)"
	@echo "built $@ ($$(stat -c%s $@) bytes, RTC boot stage)"

$(OUT)/link.ld: soc/$(SOC)/link.ld.in $(OUT)/config.stamp
	@$(PFX)gcc -E -P -x c -DNDF_BOOT_FAST=$(if $(filter-out 0,$(LOAD_AT)),1,0) -DNDF_XIP=$(XIP_NUM) -DNDF_XIP_ADDR=$(strip $(XIP_ADDR)) -DNDF_XIP_SIZE=$(strip $(XIP_SIZE)) $< -o $@

$(OUT)/app.elf: $(OBJS) $(OUT)/link.ld
	@$(PFX)gcc $(FLAGS) -nostdlib -T $(OUT)/link.ld -Wl,--gc-sections \
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
	@$(PFX)gcc $(FLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)gcc $(FLAGS) -c $(@:.o=.s) -o $@

$(OUT)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS $<"
	@$(PFX)gcc $(FLAGS) -MMD -MP -c $< -o $@

flash: all
	esptool.py --chip $(SOC) -p $(PORT) write_flash $(if $(filter-out 0,$(LOAD_AT)),0x0 $(OUT)/loader.bin 0x1000 $(OUT)/rtcboot.bin $(LOAD_AT) $(OUT)/app.bin,0x0 $(OUT)/app.bin) \
	    $(if $(filter-out 0,$(XIP_NUM)),$(strip $(XIP_ADDR)) $(OUT)/xip.bin)

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
