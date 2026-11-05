# memprot_test: Memory protection: stores into code through the IRAM address and the DRAM alias are blocked and reported (callback: bus, address, write); data stores above the split...
NDF_PATH ?= ../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_PASS = memprot_test PASS done
include $(NDF_PATH)/make/project.mk
