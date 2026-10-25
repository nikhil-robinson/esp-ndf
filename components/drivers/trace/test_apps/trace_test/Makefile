# trace_test: App trace over JTAG: waits for OpenOCD ("esp apptrace start"), streams 64 records of 1020 B (u32 seq + pattern), measures CPU cycles per trace_write and the throughput.
NDF_PATH ?= ../../../../..
TEST_KIND = manual
TEST_NOTE = needs wiring, a scope, a second board or a host script: see the source comment
include $(NDF_PATH)/make/project.mk
