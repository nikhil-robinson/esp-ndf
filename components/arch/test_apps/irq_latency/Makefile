# irq_latency: Interrupt latency in CPU cycles, per level, using the core's CCOMPARE timers: they fire at an exact CCOUNT, so latency = CCOUNT at handler entry - CCOMPARE.
NDF_PATH ?= ../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = bench
TEST_PASS = level 5 .*cycles
include $(NDF_PATH)/make/project.mk
