# stress: DMA + interrupt stress, 60 s, data checked on every transfer.
NDF_PATH ?= ../..
TEST_KIND = unit
TEST_PASS = stress PASS done
TEST_TIMEOUT = 100
TEST_NOTE = 60 s: UART DMA loopback + SPI slave/master + GDMA + 20 kHz timer + core 1; wiring docs/WIRING.md
include $(NDF_PATH)/make/project.mk
