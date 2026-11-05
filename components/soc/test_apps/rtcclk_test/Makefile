# rtcclk_test: RTC slow clock: calibration (timer group 0) against the RTC timer counted over CPU time, switch to 8 MHz/256 and back, 32 kHz crystal start on a board without one...
NDF_PATH ?= ../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_PASS = rtcclk_test PASS done
include $(NDF_PATH)/make/project.mk
