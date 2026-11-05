# sta_unit: Wi-Fi station (scan, join, data) against the IDF AP on the BOX-3 (line commands; tools/wl/test_sta.py)
NDF_PATH ?= ../../../..
COMPONENTS = wifi
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_HOST = tools/wl/test_sta.py
TEST_TIMEOUT = 150
TEST_NOTE = two boards: the IDF peer (BOX-3) runs the access point
XIP = all
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
