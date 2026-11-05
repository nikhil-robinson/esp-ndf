# psram_test: PSRAM + RF data path: gapless capture into PSRAM (rf_rx_capture_to) and triggered capture (rf_rx_capture_trigger).
NDF_PATH ?= ../../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = manual
TEST_NOTE = needs the RF data path: run with the RF tools (tools/rf/rf_pkt_test.py)
include $(NDF_PATH)/make/project.mk
