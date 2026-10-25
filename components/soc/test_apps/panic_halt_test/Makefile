# panic_halt_test: Default panic policy (NDF_PANIC_RESTART 0) halts: after a fault the interrupt watchdog must not reset the chip.
NDF_PATH ?= ../../../..
TEST_KIND = unit
TEST_PASS = panic_halt_test: fault
TEST_ALLOW = PANIC
TEST_QUIET = 8
TEST_TIMEOUT = 10
TEST_NOTE = after a fault the board must stay halted: the pass line may appear only once
include $(NDF_PATH)/make/project.mk
