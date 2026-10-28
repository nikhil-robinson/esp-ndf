# http_unit: HTTP server and client on lwIP over the Wi-Fi station, against the IDF peer (tools/wl/test_http.py)
NDF_PATH ?= ../../../..
COMPONENTS = wifi lwip net
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/components/lwip/include
TEST_KIND = unit
TEST_HOST = tools/wl/test_http.py
TEST_TIMEOUT = 300
TEST_NOTE = two boards: the IDF peer (BOX-3) runs the access point, the HTTP client and an HTTP server
XIP = all
STACK0_SIZE = 8192
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
