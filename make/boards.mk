# Boards by USB id (contains the MAC, stable across replugs). Override PORT=... for any other board.
DEVKIT = /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:D0-if00
BOX3   = /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00
# The ESP-IDF peer of the two-board tests (tools/wl, TEST_HOST apps). The second dev board, same kind as DEVKIT, replaced the BOX-3 here when the BOX-3 left the USB bus.
PEER   = /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:48-if00
