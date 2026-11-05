# wifi_mac_unit: Wi-Fi MAC driver v2 against the IDF AP on the BOX-3 (line commands; tools/wl/test_mac.py)
NDF_PATH ?= ../../../../..
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_HOST = tools/wl/test_mac.py
TEST_TIMEOUT = 120
TEST_NOTE = two boards: the IDF peer (BOX-3) runs the access point
XIP = all
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
