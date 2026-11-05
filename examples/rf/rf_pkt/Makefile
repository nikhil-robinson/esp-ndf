# rf_pkt: rf_pkt: the hardware packet engine (rf.h) driven by line commands, for tools/rf/rf_pkt_test.py (two boards). send mhz rate sync flags len count     payload 00 01 02 ...;...
NDF_PATH ?= ../../..
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
include $(NDF_PATH)/make/project.mk
