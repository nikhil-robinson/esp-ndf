# ap_unit: the esp-ndf access point (open and WPA2) with its DHCP server and lwIP, against the IDF station on the peer board (tools/wl/test_ap.py)
NDF_PATH ?= ../../../..
COMPONENTS = wifi lwip net tls
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/components/lwip/include
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_HOST = tools/wl/test_ap.py
TEST_TIMEOUT = 600
TEST_NOTE = two boards: the IDF peer runs the station (and the BOX-3 may sniff)
XIP = all
STACK0_SIZE = 8192
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
