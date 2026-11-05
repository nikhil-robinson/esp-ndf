# rtcboot_test: Does a software system reset follow RTC_CNTL_PROCPU_STAT_VECTOR_SEL (TRM 10.6) and start the CPU at 0x50000000 (RTC slow memory) without the ROM? Run 1: install...
NDF_PATH ?= ../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = manual
TEST_NOTE = needs wiring, a scope, a second board or a host script: see the source comment
include $(NDF_PATH)/make/project.mk
