# Wireless bring-up (BLE GATT, Wi-Fi, lwIP, HTTP)

Plan: [superpowers/plans/2026-10-27-wireless.md](superpowers/plans/2026-10-27-wireless.md).

| Board | Runs | Port |
|---|---|---|
| DEVKIT C9:D0 | esp-ndf (our CALL0 code) | `make/boards.mk` DEVKIT |
| BOX-3 | ESP-IDF peer `bench/idf_wl_peer` (NimBLE, esp_wifi, lwIP sockets), line commands | `make/boards.mk` BOX3 |

Two-board tests are applications with `TEST_HOST = tools/wl/<script>.py`: `tools/ndf_test.py` builds and flashes the
esp-ndf app, then runs the script with `NDF_PORT` / `PEER_PORT` in the environment; its exit code is the verdict.
`tools/wl/peer.py` (`Peer.cmd("wifi_ap T - 6")`) talks to the peer. An app lists the optional components it needs
(`COMPONENTS = ble_host wifi lwip net`); nothing else grows.
Boards sit side by side at low TX power index; esp-ndf never links IDF code (the peer is the only IDF image).
