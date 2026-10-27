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

## Status (2026-10-27)

| Item | State |
|---|---|
| Peer firmware, harness (T1, T2) | done: `bench/idf_wl_peer`, `tools/wl/{peer,ndf}.py`, `TEST_HOST` tests |
| Wi-Fi MAC findings (T3) | RX works, ring semantics known; ACK / TX-RX turnaround open: [WIFI_MAC.md](WIFI_MAC.md) |
| GATT server (T4) | done: `components/ble_host` (ATT: MTU, find info, find by type, read by type / group, read, blob, write, write cmd, notify, indicate, CCCD; 16-bit UUIDs, MTU 23). Two-board test `examples/bluetooth/ble_gatts` passes: discovery, read / write, ATT errors, 50 notifications / 5 s without a gap, graceful reconnect, abrupt link loss. Host: 600 000 fuzzed PDUs under ASan / UBSan (`make security-test`). Found and fixed in `ble_hw`: LL_TERMINATE_IND ended the link from the main loop and stalled the core |
| GATT server throughput (T4b) | **open**: 14 notifications/s vs 739/s for IDF NimBLE on the same 50 ms connection (one PDU per event; IDF chains ~37). Needs a multi-descriptor TX chain with the more-data bit in `ble_hw` |
