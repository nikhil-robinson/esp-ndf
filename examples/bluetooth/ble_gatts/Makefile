# ble_gatts: GATT server on the S3 link layer (components/ble_host)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
XIP = all
STACK1_SIZE = 4096
TEST_KIND = unit
TEST_HOST = tools/wl/test_gatts.py
TEST_TIMEOUT = 600
TEST_NOTE = two boards: the IDF peer (BOX-3) is the GATT client
include $(NDF_PATH)/make/project.mk
