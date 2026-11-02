# ble_gatts_multi: the GATT server of ble_gatts with two links at once (ble_hw_adv_multi)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/examples/bluetooth/ble_gatts
XIP = all
STACK1_SIZE = 4096
TEST_KIND = unit
TEST_HOST = tools/wl/test_gatts_multi.py
TEST_TIMEOUT = 240
TEST_NOTE = three boards: the two IDF centrals (PEER and BOX3, peer image) connect to us at once
include $(NDF_PATH)/make/project.mk
