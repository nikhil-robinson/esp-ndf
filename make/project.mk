# esp-ndf project build (ESP-IDF style): an application folder holds its sources and a three-line Makefile
#     NDF_PATH ?= ../../..
#     include $(NDF_PATH)/make/project.mk
# and is built from inside that folder:   make [flash] [monitor] [PORT=...] [DEFS="-DNDF_X=1"]
# Settings in the app Makefile (before the include), <app>/app.mk (menuconfig) or on the command line override the
# defaults below. Test applications add TEST_* keys (docs/TESTING.md).
SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c                  # the compile step pipes gcc into the loop fixer: without pipefail a compile error left an empty object and a "successful" build
NDF_PATH ?= $(error NDF_PATH is not set: the app Makefile must set it to the esp-ndf root)
NDF  := $(abspath $(NDF_PATH))
APP  := $(CURDIR)
C    := $(NDF)/components
SOC  ?= esp32s3
include $(NDF)/make/boards.mk
PORT ?= $(DEVKIT)
include $(C)/soc/$(SOC)/soc.mk
# Defaults are the S3's; another chip's soc.mk sets its own (soc/esp32p4/soc.mk): toolchain, flags, driver sources,
# lib files it replaces (SOC_LIB_EXCLUDE), include dirs searched first (SOC_INCS), image flash address. build/ for the
# S3, build-<chip>/ for other chips.
ARCH ?= xtensa
TC   ?= $(HOME)/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20250707/xtensa-esp-elf
PFX  ?= $(TC)/bin/xtensa-$(SOC)-elf-
# CALL0 ABI: no register windows -> cheap, deterministic interrupt entry.
ARCH_FLAGS ?= -mabi=call0 -mlongcalls -mtext-section-literals
SOC_DRIVER_SRCS ?= $(wildcard $(C)/drivers/*/*.c $(C)/drivers/*/*.S)
FLASH_APP_ADDR ?= 0x0
SOC_SUFFIX = $(if $(filter-out esp32s3,$(SOC)),-$(SOC))

# ---- build-side configuration (override in $(APP)/app.mk or on the command line; DEFS="-DNDF_X=1" for one-off
# option overrides, e.g. benchmark variants)
-include $(APP)/app.mk
FLASH_MODE      ?= dio        # image header: mode the ROM loader uses (the driver switches to QIO itself)
FLASH_FREQ      ?= 80m        # image header: SPI clock for the ROM loader
STACK0_SIZE     ?= 16384      # core 0 stack, bytes
STACK1_SIZE     ?= 16384      # core 1 stack, bytes
PART_TABLE_ADDR ?= 0x80000    # partition table offset; the image must end below it
OTA_XIP_OFF     ?= 0x80000   # OTA slot layout: code window (xip.bin) at slot + this
BOOT_FAST       ?= 1          # 1: the ROM loads a small loader that copies the app from BOOT_LOAD_ADDR (fast boot)
BOOT_LOAD_ADDR  ?= 0x2000     # flash offset of the app image when BOOT_FAST = 1
# loader build / flash layout (the chip's soc.mk overrides; the defaults are the S3's: loader at 0x0, RTC boot stage at 0x1000)
LOADER_SRCS     ?= loader.S loader.c ../regi2c.c
LOADER_ARCH     ?= -mabi=call0 -mtext-section-literals
LOADER_EXTRA    ?= $(OUT)/rtcboot.bin
LOADER_FLASH    ?= 0x0 $(OUT)/loader.bin 0x1000 $(OUT)/rtcboot.bin
XIP             ?= lib        # code from flash: off (all in RAM) | lib (libraries in flash) | all (libraries, app, storage)
XIP_ADDR        ?= 0x300000   # flash offset of the code blob (xip partition), 64 KB aligned
XIP_SIZE        ?= 0x100000   # its size
XIP_NUM = $(if $(filter off,$(strip $(XIP))),0,$(if $(filter all,$(strip $(XIP))),2,1))
LOAD_AT = $(if $(filter 1,$(strip $(BOOT_FAST))),$(strip $(BOOT_LOAD_ADDR)),0)
CFG_FLAGS = $(DEFS) -DNDF_BOOT_LOAD_ADDR=$(LOAD_AT) -DNDF_XIP=$(XIP_NUM) -DNDF_XIP_ADDR=$(strip $(XIP_ADDR)) -DNDF_XIP_SIZE=$(strip $(XIP_SIZE)) -DNDF_OTA_XIP_OFF=$(strip $(OTA_XIP_OFF)) -DNDF_PART_TABLE_ADDR=$(strip $(PART_TABLE_ADDR)) $(if $(wildcard $(APP)/ndf_config.h),-include $(APP)/ndf_config.h)
ifeq ($(NO_ACCEL),1)
  ACCEL_FLAGS = -DNDF_NO_ACCEL       # every accelerated path off: tests the portable fallbacks
  SOC_LOOPFIX := 0
  OUT   ?= $(APP)/build-noaccel$(SOC_SUFFIX)
else
  OUT   ?= $(APP)/build$(SOC_SUFFIX)
endif

# SDK sources (paths relative to the esp-ndf root, objects under $(OUT)/ndf/) and the application's own (objects under
# $(OUT)/app/). UNIT_MAIN = 1 links the generic test runner (make/unit_main.c) instead of an app main().
SDK_SRCS = $(patsubst $(NDF)/%,%,$(filter-out $(addprefix $(C)/lib/,$(SOC_LIB_EXCLUDE)),$(wildcard $(C)/arch/$(ARCH)/*.c $(C)/arch/$(ARCH)/*.S $(C)/soc/$(SOC)/*.c $(SOC_DRIVER_SRCS) $(C)/lib/*.c $(C)/lib/*.S $(C)/lib/musl/*.c)))
# C++ runtime (new/delete, ABI hooks) only when the app has C++ sources
# optional components an app lists in COMPONENTS (components/<c>/*.c and one level of subfolders, not test_apps/include)
SDK_SRCS += $(patsubst $(NDF)/%,%,$(foreach c,$(COMPONENTS),$(wildcard $(C)/$(c)/*.c) $(filter-out %/test_apps/% %/include/%,$(wildcard $(C)/$(c)/*/*.c))))
SDK_SRCS += $(if $(wildcard $(APP)/*.cpp),components/lib/cxx/cxx.cpp) $(if $(filter 1,$(UNIT_MAIN)),make/unit_main.c)
APP_SRCS = $(notdir $(wildcard $(APP)/*.c $(APP)/*.S $(APP)/*.cpp))
OBJS  = $(patsubst %,$(OUT)/ndf/%.o,$(SDK_SRCS)) $(patsubst %,$(OUT)/app/%.o,$(APP_SRCS))
INCS  = $(SOC_INCS) -I$(C)/include -I$(C)/arch/$(ARCH) -I$(C)/soc/$(SOC) -I$(NDF)/bench -I$(C)/lib/musl $(addprefix -I,$(EXTRA_INCLUDES)) $(foreach c,$(COMPONENTS),-I$(C)/$(c)/include)
# file names in __FILE__ / debug info are relative to the esp-ndf root (or the app): identical binaries from any checkout
PREFIX_MAPS = -ffile-prefix-map=$(NDF)/=  -ffile-prefix-map=$(APP)/=

FLAGS = $(ARCH_FLAGS) -O2 -g -ffreestanding -fno-builtin \
        -ffunction-sections -fdata-sections -fstack-protector-strong -Wformat -Wformat-security -Wall -Wextra -Wno-unused-function $(ACCEL_FLAGS) $(CFG_FLAGS) $(INCS) $(PREFIX_MAPS)

# Rebuild everything when the effective configuration changes (app.mk, command
# line, adding/removing <app>/ndf_config.h); its contents are tracked by .d files.
CFG_SIG := $(FLAGS) $(STACK0_SIZE) $(STACK1_SIZE) $(FLASH_MODE) $(FLASH_FREQ) $(XIP) $(XIP_ADDR) $(XIP_SIZE) $(BOOT_FAST) $(BOOT_LOAD_ADDR)
$(shell mkdir -p $(OUT); echo '$(CFG_SIG)' | cmp -s - $(OUT)/config.stamp || echo '$(CFG_SIG)' > $(OUT)/config.stamp)
.DEFAULT_GOAL := all
all: $(OUT)/app.bin $(if $(filter-out 0,$(LOAD_AT)),$(OUT)/loader.bin $(LOADER_EXTRA))
.PHONY: all
$(OBJS) $(OUT)/app.elf $(OUT)/app.bin: $(OUT)/config.stamp $(wildcard $(APP)/ndf_config.h)

$(OUT)/app.bin: $(OUT)/app.elf
	@$(PFX)objcopy -R .flash.text -R .flash.rodata $< $(OUT)/app_ram.elf 2>&1 | grep -v "empty loadable segment" >&2 || true   # the ROM loads only the RAM part
ifeq ($(ARCH),riscv)
	@# RTC_DATA initial values: the ROM would write a .rtc.data section into LP RAM at every boot (and deep-sleep wake); ship them as a section at their load address in L2MEM instead (sys_start0 copies them)
	@$(PFX)objcopy -O binary -j .rtc.data $< $(OUT)/rtc_data.bin
	@if [ -s $(OUT)/rtc_data.bin ]; then $(PFX)objcopy -R .flash.text -R .flash.rodata -R .rtc.data --add-section .rtc.init=$(OUT)/rtc_data.bin --set-section-flags .rtc.init=alloc,load,contents,data --change-section-address .rtc.init=0x$$($(PFX)nm $< | awk '$$3=="_rtc_data_load"{print $$1}') $< $(OUT)/app_ram.elf 2>&1 | grep -v "empty loadable segment" >&2 || true; fi
endif
	esptool.py --chip $(SOC) elf2image --flash_mode $(strip $(FLASH_MODE)) --flash_freq $(strip $(FLASH_FREQ)) -o $@ $(OUT)/app_ram.elf >/dev/null
	@$(PFX)objcopy -O binary -j .flash.text $< $(OUT)/xip_text.bin; $(PFX)objcopy -O binary -j .flash.rodata $< $(OUT)/xip_ro.bin
	@cat $(OUT)/xip_text.bin $(OUT)/xip_ro.bin > $(OUT)/xip.bin                  # code blob for XIP_ADDR (rodata right after the code)
	@test $$(stat -c%s $(OUT)/xip.bin) -le $$(( $(strip $(XIP_SIZE)) )) || (echo "xip.bin larger than XIP_SIZE"; rm -f $@; false)
	@python3 $(NDF)/tools/mkunwind$(if $(filter xtensa,$(ARCH)),,_rv).py $(PFX)readelf $(PFX)nm $< $@ >/dev/null   # exact panic backtraces: unwind table in flash
	@test $$(( $$(stat -c%s $@) + $(LOAD_AT) )) -le $$(( $(strip $(PART_TABLE_ADDR)) )) || (echo "image larger than PART_TABLE_ADDR ($(strip $(PART_TABLE_ADDR))): would overwrite the partition table"; rm -f $@; false)
	@python3 -c "import sys; a=open('$@','rb').read(); x=open('$(OUT)/xip.bin','rb').read(); o=$(strip $(OTA_XIP_OFF)); assert len(a)<=o, 'app.bin larger than OTA_XIP_OFF'; open('$(OUT)/ota.bin','wb').write(a+b'\\xff'*(o-len(a))+x if x else a)"   # OTA image
	@echo "built $@"

LOADER_FILES = $(addprefix $(C)/soc/$(SOC)/loader/,$(LOADER_SRCS))
$(OUT)/loader.bin: $(LOADER_FILES) $(C)/soc/$(SOC)/loader/loader.ld $(OUT)/config.stamp
	@$(PFX)gcc $(LOADER_ARCH) -Os -ffreestanding -nostdlib -ffunction-sections -fdata-sections $(INCS) $(CFG_FLAGS) \
	    $(if $(BOOT_TRACE),-DNDF_BOOT_TRACE=1) -Wl,--gc-sections -T $(C)/soc/$(SOC)/loader/loader.ld -o $(OUT)/loader.elf \
	    $(LOADER_FILES)   # S3: analog-register access shared with the app
	@esptool.py --chip $(SOC) elf2image --flash_mode $(strip $(FLASH_MODE)) --flash_freq $(strip $(FLASH_FREQ)) -o $@ $(OUT)/loader.elf >/dev/null
	@echo "built $@ ($$(stat -c%s $@) bytes; the ROM loads only this)"

# RTC boot stage: the same loader, run from RTC fast memory behind a jump at the CPU reset vector 0x50000000 (RTC slow
# memory, slow to fetch from; loader_rtc.ld). The flash loader copies it
# there on a cold boot and points the reset vector at it: warm resets and deep-sleep wakes then boot without the ROM.
$(OUT)/rtcboot.bin: $(C)/soc/$(SOC)/loader/loader.S $(C)/soc/$(SOC)/loader/loader.c $(C)/soc/$(SOC)/loader/loader_rtc.ld $(C)/soc/$(SOC)/regi2c.c $(OUT)/config.stamp
	@$(PFX)gcc -mabi=call0 -mtext-section-literals -Os -ffreestanding -nostdlib -ffunction-sections -fdata-sections $(INCS) $(CFG_FLAGS) \
	    -DNDF_LOADER_RTC=1 $(if $(BOOT_TRACE),-DNDF_BOOT_TRACE=1) -Wl,--gc-sections -T $(C)/soc/$(SOC)/loader/loader_rtc.ld -o $(OUT)/rtcboot.elf \
	    $(C)/soc/$(SOC)/loader/loader.S $(C)/soc/$(SOC)/loader/loader.c $(C)/soc/$(SOC)/regi2c.c
	@test $$($(PFX)size -A $(OUT)/rtcboot.elf | awk '$$1==".data"{d=$$2} END{print d+0}') -eq 0 || (echo "rtcboot: .data must be empty (copied raw)"; false)
	@$(PFX)objcopy -O binary -j .rtc $(OUT)/rtcboot.elf $(OUT)/rtcboot_jmp.raw
	@$(PFX)objcopy -O binary -j .text $(OUT)/rtcboot.elf $(OUT)/rtcboot.raw
	@python3 -c "import struct; j,d=(open('$(OUT)/rtcboot'+n,'rb').read() for n in ('_jmp.raw','.raw')); j+=b'\\0'*(-len(j)%4); d+=b'\\0'*(-len(d)%4); open('$@','wb').write(struct.pack('<III',0x42435452,len(j),len(d))+j+d)"
	@echo "built $@ ($$(stat -c%s $@) bytes, RTC boot stage)"

$(OUT)/link.ld: $(C)/soc/$(SOC)/link.ld.in $(OUT)/config.stamp
	@$(PFX)gcc -E -P -x c $(DEFS) $(if $(wildcard $(APP)/ndf_config.h),-include $(APP)/ndf_config.h) -DNDF_BOOT_FAST=$(if $(filter-out 0,$(LOAD_AT)),1,0) -DNDF_XIP=$(XIP_NUM) -DNDF_XIP_ADDR=$(strip $(XIP_ADDR)) -DNDF_XIP_SIZE=$(strip $(XIP_SIZE)) $< -o $@

$(OUT)/app.elf: $(OBJS) $(OUT)/link.ld
	@$(PFX)gcc $(FLAGS) -nostdlib -T $(OUT)/link.ld -Wl,--gc-sections \
	    -Wl,--defsym=NDF_STACK0_SIZE=$(strip $(STACK0_SIZE)) -Wl,--defsym=NDF_STACK1_SIZE=$(strip $(STACK1_SIZE)) -Wl,-Map=$(OUT)/app.map -o $@ $(OBJS)
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

# imported musl math (components/lib/musl/README.md): its own conventions, no warnings
$(OUT)/ndf/components/lib/musl/%.c.o: FLAGS += -include $(C)/lib/musl/features.h -w
# libm entry points are pure arithmetic on scalars (no buffer, no external length): a stack canary there only costs
# cycles (sinf 218 -> 198, atan2f 291 -> 272); every other file keeps -fstack-protector-strong
$(OUT)/ndf/components/lib/math.c.o $(OUT)/ndf/components/lib/mathf.c.o: FLAGS += -fno-stack-protector

$(OUT)/ndf/%.c.o: $(NDF)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC $*.c"
	@$(PFX)gcc $(FLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)gcc $(FLAGS) -c $(@:.o=.s) -o $@

$(OUT)/app/%.c.o: $(APP)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC $*.c"
	@$(PFX)gcc $(FLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)gcc $(FLAGS) -c $(@:.o=.s) -o $@

# C++: no exceptions / RTTI / thread-safe statics (no libstdc++: components/lib/cxx/cxx.cpp has new/delete and the ABI hooks)
CXXFLAGS = $(filter-out -fno-builtin,$(FLAGS)) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
$(OUT)/ndf/%.cpp.o: $(NDF)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX $*.cpp"
	@$(PFX)g++ $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)g++ $(CXXFLAGS) -c $(@:.o=.s) -o $@

$(OUT)/app/%.cpp.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX $*.cpp"
	@$(PFX)g++ $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -S $< -o - | $(LOOPFIX) > $(@:.o=.s)
	@$(PFX)g++ $(CXXFLAGS) -c $(@:.o=.s) -o $@

$(OUT)/ndf/%.S.o: $(NDF)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS $*.S"
	@$(PFX)gcc $(FLAGS) -MMD -MP -c $< -o $@

$(OUT)/app/%.S.o: $(APP)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS $*.S"
	@$(PFX)gcc $(FLAGS) -MMD -MP -c $< -o $@

flash: all
	esptool.py --chip $(SOC) -p $(PORT) write_flash $(if $(filter-out 0,$(LOAD_AT)),$(LOADER_FLASH) $(LOAD_AT) $(OUT)/app.bin,$(FLASH_APP_ADDR) $(OUT)/app.bin) \
	    $(if $(filter-out 0,$(XIP_NUM)),$(strip $(XIP_ADDR)) $(OUT)/xip.bin)

PARTS ?= $(NDF)/tools/partitions.csv
partition:
	python3 $(NDF)/tools/mkpart.py $(PARTS) $(OUT)/partitions.bin
	esptool.py --chip $(SOC) -p $(PORT) write_flash $(strip $(PART_TABLE_ADDR)) $(OUT)/partitions.bin

# decode a panic backtrace:  make backtrace ADDR="0x4037.. 0x4037.."
backtrace:
	@$(PFX)addr2line -pfiaC -e $(OUT)/app.elf $(ADDR)

menuconfig:
	python3 $(NDF)/tools/ndfconfig/ndfconfig.py $(APP)

config-doc:
	python3 $(NDF)/tools/ndfconfig/ndfconfig.py --doc $(NDF)/docs/CONFIG.md

monitor:
	python3 -m serial.tools.miniterm $(PORT) 115200

clean:
	rm -rf $(APP)/build $(APP)/build-noaccel

# test applications: flash, run and judge this one app (tools/ndf_test.py; keys in docs/TESTING.md)
test:
	python3 $(NDF)/tools/ndf_test.py run $(APP)
ndf-info:
	@echo "TEST_KIND=$(or $(TEST_KIND),example)"; echo "TEST_BOARD=$(or $(TEST_BOARD),devkit)"; echo "TEST_PASS=$(or $(TEST_PASS),=== unit: (\\d+)/(\\d+) passed ===)"
	@echo "TEST_TIMEOUT=$(or $(TEST_TIMEOUT),60)"; echo "TEST_HOST=$(TEST_HOST)"; echo "TEST_NOTE=$(TEST_NOTE)"; echo "TEST_ALLOW=$(TEST_ALLOW)"; echo "TEST_QUIET=$(or $(TEST_QUIET),0)"; echo "TEST_SOC=$(TEST_SOC)"

-include $(OBJS:.o=.d)
.PHONY: flash monitor clean partition backtrace menuconfig config-doc test ndf-info
