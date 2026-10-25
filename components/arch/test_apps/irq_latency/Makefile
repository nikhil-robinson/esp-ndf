# irq_latency: Interrupt latency in CPU cycles, per level, using the core's CCOMPARE timers: they fire at an exact CCOUNT, so latency = CCOUNT at handler entry - CCOMPARE.
NDF_PATH ?= ../../../..
TEST_KIND = bench
TEST_PASS = level 5 .*cycles
include $(NDF_PATH)/make/project.mk
