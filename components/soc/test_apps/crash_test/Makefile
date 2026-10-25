# crash_test: Crash handling over real restarts (NDF_PANIC_RESTART): step 0 dies on an illegal instruction, step 1 smashes a stack canary (-fstack-protector-strong), step 2 masks...
NDF_PATH ?= ../../../..
TEST_KIND = unit
TEST_PASS = crash_test done
TEST_ALLOW = PANIC
TEST_TIMEOUT = 120
TEST_NOTE = resets itself 3 times (illegal instruction, stack canary, interrupt watchdog)
include $(NDF_PATH)/make/project.mk
