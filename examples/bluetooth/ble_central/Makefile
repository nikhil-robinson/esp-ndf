# ble_central: the esp-ndf device as BLE central: initiates a connection to an advertiser, then runs the GATT client (components/ble_host)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/examples/bluetooth/ble_gattc
XIP = all
STACK1_SIZE = 4096
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_HOST = tools/wl/test_central.py
TEST_TIMEOUT = 150
TEST_NOTE = two boards: the IDF peripheral (PEER) hosts the GATT server we query
include $(NDF_PATH)/make/project.mk
