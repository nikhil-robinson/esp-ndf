# https_unit: HTTPS server and client (TLS 1.3, our own engine) on lwIP over the Wi-Fi station, against the IDF peer's esp_https_server / esp_http_client (tools/wl/test_https.py)
NDF_PATH ?= ../../../..
COMPONENTS = wifi lwip net tls
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf $(NDF_PATH)/components/lwip/include
TEST_KIND = unit
TEST_HOST = tools/wl/test_https.py
TEST_TIMEOUT = 400
TEST_NOTE = two boards: the IDF peer runs the AP, an HTTPS client and an HTTPS server (certificates from tools/wl/pki)
XIP = all
STACK0_SIZE = 8192
STACK1_SIZE = 2048
DEFS = -DNDF_NET_MEM_SIZE=8192 -DNDF_NET_PBUFS=5 -DNDF_NET_TCP_MSS=536      # RAM: the TLS state and the elliptic-curve code come out of the 160 KB below the RF dump engine banks
include $(NDF_PATH)/make/project.mk
