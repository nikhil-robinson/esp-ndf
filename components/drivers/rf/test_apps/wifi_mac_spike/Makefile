# wifi_mac_spike: what the Wi-Fi MAC does without a vendor stack (line commands, docs/WIFI_MAC.md)
NDF_PATH ?= ../../../../..
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = manual
TEST_NOTE = driven by tools/wl/spike_mac.py with the IDF peer
XIP = all
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
