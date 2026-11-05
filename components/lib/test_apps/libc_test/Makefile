# libc_test: libc + float runtime: divide/sqrt/conversions bit-exact against host IEEE (numpy) vectors, printf/snprintf formats against C reference strings, strtol/strtof,...
NDF_PATH ?= ../../../..
TEST_KIND = unit
TEST_PASS = === ALL PASS \(0 failures\) ===
ifeq ($(SOC),esp32p4)
XIP ?= all            # the P4 v1.x image window is 175 KB: code runs from flash
endif
include $(NDF_PATH)/make/project.mk
