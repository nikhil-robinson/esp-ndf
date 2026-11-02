# ble_multi: the esp-ndf device as BLE central with two links at once (components/ble_host, ble_hw links 0 and 1)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
XIP = all
STACK1_SIZE = 4096
TEST_KIND = unit
TEST_HOST = tools/wl/test_multi.py
TEST_TIMEOUT = 240
TEST_NOTE = three boards: two IDF peripherals (PEER and BOX3, both with the peer image) host the GATT servers we connect to
include $(NDF_PATH)/make/project.mk
