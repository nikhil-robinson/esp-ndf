# sys_test: Ring buffer, async DMA copy, watchdog and panic report.
NDF_PATH ?= ../../../..
TEST_KIND = unit
TEST_PASS = === ALL PASS \(0 failures\) ===
TEST_TIMEOUT = 120
TEST_ALLOW = PANIC
TEST_NOTE = three boots: ring buffer, async DMA, watchdog, fault + restart
include $(NDF_PATH)/make/project.mk
