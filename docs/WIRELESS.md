# Wireless bring-up (BLE GATT, Wi-Fi, lwIP, HTTP)

Plan: [superpowers/plans/2026-10-27-wireless.md](superpowers/plans/2026-10-27-wireless.md).

| Board | Runs | Port |
|---|---|---|
| DEVKIT C9:D0 | esp-ndf (our CALL0 code) | `make/boards.mk` DEVKIT |
| second dev board C9:48 | ESP-IDF peer `bench/idf_wl_peer` (NimBLE, esp_wifi, lwIP sockets, esp_http_client, esp_https_server, mbedtls), line commands | `make/boards.mk` PEER |
| BOX-3 | the same peer image, used as the sniffer (`wifi_mon`, `wifi_watch`, `wifi_bcn`) and as a second IDF station / server | `make/boards.mk` BOX3 |

Two-board tests are applications with `TEST_HOST = tools/wl/<script>.py`: `tools/ndf_test.py` builds and flashes the
esp-ndf app, then runs the script with `NDF_PORT` / `PEER_PORT` in the environment; its exit code is the verdict.
`tools/wl/peer.py` (`Peer.cmd("wifi_ap T - 6")`) talks to the peer. An app lists the optional components it needs
(`COMPONENTS = ble_host wifi lwip net`); nothing else grows.
Boards sit side by side at low TX power index; esp-ndf never links IDF code (the peer is the only IDF image). Every two-board suite reboots the peer first (`Peer.reboot()`): its AP and DHCP server degrade after many restarts inside one boot.

## Status (2026-10-29)

| Item | State |
|---|---|
| Peer firmware, harness (T1, T2) | done: `bench/idf_wl_peer`, `tools/wl/{peer,ndf}.py`, `TEST_HOST` tests, sniffer with timestamps in both directions |
| Wi-Fi MAC (T3, T6) | `wifi_mac` driver on the S3's own MAC: software FCS, TX/RX switching by MAC reset (~2.3 ms), channels 1-13, TX rates 1-36 Mbit/s, receives 802.11b and 11g OFDM (6-54 Mbit/s, IDF's rx_state test); **hardware ACK works** (address filter; IDF's rx-queue policy bit must stay clear), hardware CCMP receive through the key table (one slot per key, 8 slots), TX parameter RAM zeroed at every TX start. Details and every measurement: [WIFI_MAC.md](WIFI_MAC.md) |
| Wi-Fi station (T7) | done: scan, open join (0.3-0.8 s), data both ways, link-down on beacon loss, deauth rules; fuzzed parsers |
| WPA2 station (T9, T10) | done: PBKDF2/PRF/MIC/RFC 3394, 4-way handshake and group rekey, wrong passphrase refused (-3), CCMP receive in the MAC key table, CCMP transmit in software; DHCP, ping 20/20, UDP ~93%, TCP 64 KB over WPA2 (`WL_PASS=... tools/wl/test_ip.py`). Forged / replayed message 3 is rejected (`sta_unit`, `wpa_unit`) |
| PMF and WPA3 station | done, as IDF's supplicant: AKM SAE > PSK-SHA256 > PSK; RSN element with MFPC (MFPR for SAE), PMKID after SAE, BIP-CMAC-128 when the AP offers PMF; KDF-SHA256 PTK, AES-CMAC EAPOL MIC, IGTK; unicast deauth / disassoc / action only when CCMP-protected, group ones only with a valid MMIE; SA Query answered; deauthentication before leaving; association comeback (status 30) honoured. SAE group 19, hunting-and-pecking (40 rounds), anti-clogging token. Vectors: RFC 4493, 802.11-2020 Annex J.10 (`wpa_unit`); two boards against the IDF AP in WPA2+PMF, PMF required, WPA3 and WPA2/WPA3 modes, forged frames from the BOX-3 (`tools/wl/test_pmf.py`). Not done: SAE hash-to-element (H2E), WPA3 on our AP |
| DNS and SNTP clients | done: `net_dns()` (lwIP resolver, server from DHCP, cache), `net_sntp()` (RFC 4330, one exchange, clock set with half the round trip; reply must come from port 123, be mode 4 with a stratum, and echo our transmit timestamp, stricter than lwIP's default that IDF uses). `tools/wl/test_dns_sntp.py`: name, cache, NXDOMAIN, wrong DNS id ignored, unechoed SNTP ignored, kiss of death refused; clock within 0.5 s of the server |
| Wi-Fi access point (T11) | done: `components/wifi/ap.c` beacons on a 102.4 ms grid (jitter 313-486 us, IDF softAP 512-1103 us), probe / auth / assoc, WPA2-PSK authenticator (one fixed group key), per-station key slot, DHCP server (`components/net/dhcpd.c`), `NDF_WIFI_MAX_STA` stations (status 17 beyond), stateless open-system auth so junk cannot fill the table, half-open stations freed after 5 s, idle stations after 300 s. No power-save buffering (stations must run with power save off). `ap_unit` + `tools/wl/test_ap.py` open and WPA2; `fuzz_ap` in `make security-test` |
| IP (T8) | `components/lwip` (lwIP 2.x raw API, NO_SYS), STA and AP netifs. DHCP 1.6 s, ping 20/20 both ways, UDP echo 300/300, TCP echo 64 KB intact in 2.3 s (open) / 15.4 s (WPA2) |
| HTTP (T12) | `components/net`: server (routes, keep-alive, fixed pool, 400 on bad requests) and client (Content-Length, chunked); `http_unit` passes. Throughput equal to `esp_http_server` on the same air (air-limited) |
| TLS 1.3 / HTTPS (T13) | `components/tls` (own engine: HKDF, record layer, X.509 subset, client and server, X25519, ECDSA P-256, AES-128-GCM) and `components/net/https.c`. Interoperates with OpenSSL (host, 1 MB both ways), with `esp_http_client` and `esp_https_server` over the air in both directions, wrong CA and wrong host name refused. X25519 2.4x, ECDSA sign 5.5x, verify 5.4x, AES-GCM 2.6x faster than mbedtls on the same chip ([PERFORMANCE.md](PERFORMANCE.md)). Limits: one TLS connection at a time, records <= 4096 bytes (a peer sending 16 KB records gets `record_overflow`), no HelloRetryRequest / PSK / client certificates, certificate validity dates are not checked (no RTC) |
| GATT server (T4) | done: `components/ble_host` (ATT: MTU, find info, find by type, read by type / group, read, blob, write, write cmd, notify, indicate, CCCD; 16-bit UUIDs, MTU 23). Two-board test `examples/bluetooth/ble_gatts`; 600 000 fuzzed PDUs under ASan / UBSan. Found and fixed in `ble_hw`: LL_TERMINATE_IND ended the link from the main loop and stalled the core |
| GATT client (T5) | done: `components/ble_host/gattc.c` over the existing link (the LL stays slave); fuzzed (`fuzz_gattc`) |
| GATT server throughput (T4b) | **open**: 14 notifications/s vs 739/s for IDF NimBLE on the same 50 ms connection (one PDU per event; IDF chains ~37). Needs a multi-descriptor TX chain with the more-data bit in `ble_hw` |

## Open items

1. **Wi-Fi transmit engine: solved** (2026-10-29): frames go through the MAC's frame-exchange engine as in IDF (carrier sense, ACK wait, 7 retries); TCP 64 KB 2.3 s open (was 95 s), 15.4 s WPA2. WPA2 still uses software CCMP on transmit (next: the hardware path). Details: [WIFI_MAC.md](WIFI_MAC.md).
2. **GATT server throughput** (T4b above).
3. **One-hour soak gate not met** (`tools/wl/soak_wl.py`, log `bench/soak/data/wireless.log`, 2026-10-29): station WPA2 25 min / 90 rounds: no UDP or TCP failure, 4 rounds with ping a little below 16/20, heap steady (72 B, one-time 152 B); access point 15 min / 310 rounds: 5 TCP echo failures (1.6 %, the TX-engine item) and 5 rounds where the station was briefly missing from the table, heap flat; BLE GATT server 20 min: 52 clean rounds (>= 94 notifications per 10 s, no gap), then `ble_disc` of the IDF central timed out (the service-discovery request got no answer) and the peer stayed stuck on connect errors until reboot; a rerun failed the same way at round 13. The board kept running and advertising, so this is an intermittent loss of an ATT response near connection start in `ble_hw` (or the peer's central), not yet analysed.
4. BLE and Wi-Fi do not run at the same time (one radio, no coexistence tuning); power save, BLE central role and LE pairing are not in v1. WPA3 is station-only (our AP is WPA2).

## Tests

| Command | What |
|---|---|
| `python3 tools/ndf_test.py run components/wifi/test_apps/{wifi_mac_unit,sta_unit,wpa_unit,ap_unit}` | MAC, station, WPA2 primitives, access point |
| `python3 tools/ndf_test.py run components/lwip/test_apps/ip_unit` (`WL_PASS=<pass> python3 tools/wl/test_ip.py` for WPA2) | IP over the station |
| `python3 tools/ndf_test.py run components/net/test_apps/{http_unit,https_unit}` | HTTP and HTTPS against the IDF peer |
| `python3 tools/ndf_test.py run components/tls/test_apps/tls_unit` | TLS on the board's crypto engines |
| `make security-test` | host fuzzers (ATT, GATT client, Wi-Fi frames, EAPOL, access point, TLS under ASan / UBSan) and the TLS engine against OpenSSL |
| `python3 tools/wl/bench_ap.py`, `bench_http.py` | AP and HTTP comparisons with IDF |
