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
OUT   = build/$(notdir $(APP))

SRCS  = $(wildcard arch/$(ARCH)/*.c arch/$(ARCH)/*.S soc/$(SOC)/*.c drivers/*/*.c lib/*.c $(APP)/*.c)
OBJS  = $(patsubst %,$(OUT)/%.o,$(SRCS))
INCS  = -Iinclude -Iarch/$(ARCH) -Isoc/$(SOC) -Ibench

# CALL0 ABI: no register windows -> cheap, deterministic interrupt entry.
FLAGS = -mabi=call0 -mtext-section-literals -O2 -g -ffreestanding -fno-builtin \
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-function $(INCS)

$(OUT)/app.bin: $(OUT)/app.elf
	esptool.py --chip $(SOC) elf2image --flash_mode dio --flash_freq 80m -o $@ $< >/dev/null
	@echo "built $@"

$(OUT)/app.elf: $(OBJS) soc/$(SOC)/link.ld
	@$(PFX)gcc $(FLAGS) -nostdlib -T soc/$(SOC)/link.ld -Wl,--gc-sections -o $@ $(OBJS)
	$(PFX)size $@

$(OUT)/%.o: %
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(PFX)gcc $(FLAGS) -MMD -c $< -o $@

flash: $(OUT)/app.bin
	esptool.py --chip $(SOC) -p $(PORT) write_flash 0x0 $<

monitor:
	python3 -m serial.tools.miniterm $(PORT) 115200

clean:
	rm -rf build

-include $(OBJS:.o=.d)
.PHONY: flash monitor clean
