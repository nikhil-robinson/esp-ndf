# ble_gattc: GATT client over the connection a central opened to us (components/ble_host)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
XIP = all
STACK1_SIZE = 4096
TEST_KIND = unit
TEST_HOST = tools/wl/test_gattc.py
TEST_TIMEOUT = 90
TEST_NOTE = two boards: the IDF central also hosts the GATT server we query
include $(NDF_PATH)/make/project.mk
