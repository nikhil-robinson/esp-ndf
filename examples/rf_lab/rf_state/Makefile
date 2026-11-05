# rf_state: Persistent-state hunt: snapshot RTC_CNTL, SENS and every analog register (blocks 0x61..0x6D, regs 0..0x3F) after our init + TX on, then do an RTC_CNTL system reset...
NDF_PATH ?= ../../..
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
include $(NDF_PATH)/make/project.mk
