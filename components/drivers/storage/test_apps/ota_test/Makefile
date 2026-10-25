# ota_test: OTA end to end over real resets (step in RTC memory): factory installs itself into ota_0 -> ota_0 first boot confirms, installs into ota_1 -> ota_1 does not confirm,...
NDF_PATH ?= ../../../../..
TEST_KIND = unit
TEST_PASS = ota_test done
TEST_TIMEOUT = 180
TEST_NOTE = installs itself into ota_0 over several real resets
include $(NDF_PATH)/make/project.mk
