# ble_scan: Hardware BLE scan, no vendor code: our PHY init, then the S3's own link-layer core receives advertising PDUs on channel 37 by itself (demodulation, access address,...
NDF_PATH ?= ../../..
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
include $(NDF_PATH)/make/project.mk
