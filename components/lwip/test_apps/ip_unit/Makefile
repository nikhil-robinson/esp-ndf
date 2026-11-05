# ip_unit: lwIP on the Wi-Fi station (DHCP, ICMP, UDP, TCP) against the IDF AP and its network services (tools/wl/test_ip.py)
NDF_PATH ?= ../../../..
COMPONENTS = wifi lwip
EXTRA_INCLUDES = $(NDF_PATH)/components/drivers/rf
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_HOST = tools/wl/test_ip.py
TEST_TIMEOUT = 600
TEST_NOTE = two boards: the IDF peer (BOX-3) runs the access point, DHCP server and the echo services
XIP = all
STACK0_SIZE = 8192
STACK1_SIZE = 4096
include $(NDF_PATH)/make/project.mk
