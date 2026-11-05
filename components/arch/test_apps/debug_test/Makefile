# debug_test: Profiling + debug: perf counters (cycles vs CCOUNT, instructions and loads of a known loop, read cost), a write watchpoint (callback with the store's PC, the store...
NDF_PATH ?= ../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_PASS = debug_test PASS done
include $(NDF_PATH)/make/project.mk
