# ble_central: the esp-ndf device as BLE central: initiates a connection to an advertiser, then runs the GATT client (components/ble_host)
NDF_PATH ?= ../../..
COMPONENTS = ble_host
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/examples/bluetooth/ble_gattc
XIP = all
STACK1_SIZE = 4096
# work in progress (docs/WIRELESS.md): initiating works against an esp-ndf slave, the master data exchange and the IDF peripheral do not yet; no automatic test until they pass
include $(NDF_PATH)/make/project.mk
