# Wi-Fi MAC on the ESP32-S3 without the vendor stack - findings

Measured with `components/drivers/rf/test_apps/wifi_mac_spike` (DEVKIT) against the IDF peer (`bench/idf_wl_peer`, BOX-3), 2026-10-27.
Register layout follows the ESP32 MAC (esp32-open-mac, plain ESP32 only) with the S3 base `0x60033000`; offsets confirmed on the S3 are marked.

| What | Where / value | Status |
|---|---|---|
| MAC address filter slot 0 / 1 | `0x60033040/44`, `0x60033048/4C` (addr[0..3], addr[4..5]) | present in the captured vendor state (holds the board MAC) |
| filter mask / ACK-enable | `0x60033060/68` = 0xFFFFFFFF; `0x60033064/6C`, bit 16 = enable (value 0x0001FFFF) | present; **effect on ACKs not yet demonstrated** |
| BSSID filter slot 0 | `0x60033000/04` value, `0x60033020/24` mask + enable (bit 16) | present (value 0, disabled in the vendor RX state) |
| RX ring head | `0x60033088` (copies at `..8C/90/94/98`), low 20 bits of the first descriptor | works (rf_wifi.c) |
| RX descriptor word0 | bit 31 owner (stays set), **bit 30 = MAC set it: frame complete**, bits 23:12 = length (= frame length + 45, includes the 48-byte header), bits 11:0 = buffer size | confirmed |
| RX header (48 B) | word0 bits 7:0 RSSI (int8), 12:8 rate (0 = 1 Mbit/s long preamble), word 11 bits 11:0 = frame length | confirmed on beacons (rate 0, -8 dBm, length 155) |
| RX buffers | must hold a whole frame: with 256-byte buffers long (encrypted) frames spill into following descriptors that carry no header (looked like garbage). 8 x 1664 B now | confirmed |
| TX | PLCP0 `0x60033D08` (descriptor low 20 bits, bit 31/30 = start), PLCP1 `0x600342F8` (len, rate), HTSIG `0x60034300`, CONF `0x60033D04`, done flag `0x60033C3C` bit 1 | works (4.2) |
| DSSS 1M reception | beacons from the IDF AP at 100 ms | works |
| FCS | **the TX path does not append the frame check sequence**: the caller adds IEEE CRC-32 (LE) after the frame. Without it the peer's sniffer still lists the frame (length = bytes sent) but the AP MLME drops it | confirmed: with FCS the IDF AP raised probe-request events and answered authentication |
| TX to RX switch | `rf_wifi_rx_init` after a transmit left the receiver dead until the MAC is reset: pulse `SYSTEM_WIFI_RST` (0x60026018) bit 2 before each replay (now in `rf_wifi_tx_init` / `rf_wifi_rx_init`); ~670 us | confirmed (register-by-register restore did not help) |
| RX length | descriptor length = frame length + 44 **rounded up to a multiple of 8** (155 -> 200, 156 -> 200, 137 -> 184). A strict check hid the IDF AP beacons, and a +-1 tolerance still dropped frames such as a 137-byte HTTP response. `rf_wifi_rx` accepts [len+44, len+52) and skips continuation buffers | confirmed |
| Open-system auth | request (1 Mbit/s, FCS added) -> IDF AP answers auth seq 2 status 0, retransmits it (retry bit) ~every 5 ms because no hardware ACK is sent | confirmed (spike `auth <bssid> <mac> auth fcs`) |
| TX rate | PLCP1 `0x600342F8` bits 12+ = IDF `wifi_phy_rate_t` code. Heard by the IDF AP: 1M 0x0, 6M 0xB, 9M 0xF, 12M 0xA, 18M 0xE, 24M 0x9, 36M 0xD; 48M 0x8 and 54M 0xC not heard | confirmed |
| TX length | DSSS frames over about 1200 B are not heard (1200 ok, 1250 not); OFDM 1500 B frames are | confirmed; cause unknown |
| RX modulation | only 1 Mbit/s DSSS is received: beacons of an AP fixed to 6/9/12/24M never arrive (the captured RX state is the DSSS one) | confirmed; OFDM receive is open |
| Hardware ACK | not working: with the filter slot 0 = our MAC and enable (0x60033064 bit 16) or the BSSID slot set, the AP still retransmits about half of its frames (retry bit). The radio is in RX mode, so no TX chain is up for an ACK | open |
| Collisions without ACK | the peer repeats a frame for ~4 ms; transmitting inside that window collides with it. `wifi_mac_tx` waits until 4 ms after the last frame addressed to us (`WIFI_QUIET_US`): peer pings 4/20 -> 19-20/20 | confirmed |
| WPA2 station | 4-way handshake (messages 1-4, group rekey) works against the IDF AP: join 0.5-1.1 s, wrong passphrase -> `-3` in 2.4 s, key install only after a valid message 3. **Open**: after it, no encrypted unicast frame from the AP reaches us (ARP / ICMP / DHCP replies), while group frames decrypt fine and the AP decrypts and forwards ours. An IDF station on the same AP (fixed 1M, WPA2) gets DHCP and pings 5/5. Not a rate problem (all of 1/2/5.5/11M are received); AP-side TX could not be observed | open |
| Hardware key table | slot n at `0x60034400 + 0x28*n` (IDF used slots 0 = pairwise, 4 = group): transmitter address (6 B), flag halfword (`0x48cc` pairwise, `0x086c` group; CCMP = 3 in the algorithm field), 16-byte key; `0x60033800` (interface 0 crypto control) goes `0x30000 -> 0x30103`, `0x60033814` has one enable bit per slot. The MAC reset in `rf_wifi_rx_init` clears all of it. Programmed identically, protected unicast frames from the AP still do not arrive | open; dumps in `bench/wifi_regs/data` |
| Key table, tried | pairwise + group slots written persistently (reinstalled after every TX/RX switch), all eight word/byte orders of the key, flag bit flips, IDF's whole interface-0 configuration cloned: no encrypted unicast frame from the AP ever reaches the ring (group frames do). Needed to separate hardware drop from PHY decode: a third radio sniffing the AP while another station talks to it | open |
| WPA2 downlink, isolated (3 radios) | with the BOX-3 sniffing: the IDF AP does transmit the protected unicast replies to our MAC (ARP reply 80 B, ping requests 144 B, 1 Mbit/s, once each, no retries), and a unicast CCMP frame encrypted on the host with our own pairwise key and injected by the AP board is also on the air (verified by the sniffer). Our MAC never delivers either to any ring, with the key table programmed like IDF (slots 0 and 4, state words cleared, `0x60033800 = 0x30103`, `0x60033814 = 0x11`), IDF's interface-0 BSSID/filter settings cloned, any of 8 key byte orders. Group frames and unprotected unicast arrive. So the MAC's crypto stage consumes them; what IDF does differently is still unknown. Next step: watch IDF's writes to the key RAM and crypto registers with a JTAG data watchpoint during an IDF station join | open |
| RX queues | `0x60033088` receives nearly every frame on the air including noise (this is where the junk frames come from), the other heads are other queues; IDF's heads all point into one descriptor pool | noted |
| Peer AP quirk | after several `wifi_stop` / `wifi_ap` cycles the IDF AP's DHCP server stopped answering; `wifi_ap` now restarts it | worked around |

## Open (blocks Wi-Fi STA/AP)

1. **TX/RX turnaround solved in software (~700 us + 1 ms settle); hardware ACK still missing** (the AP retries every unacked frame). `rf_wifi_tx_init` and `rf_wifi_rx_init` each retune and replay a whole register set (ms); after one TX the receiver is dead until `rf_wifi_rx_init` runs again. A Wi-Fi ACK is due 10 us after a frame, so ACK transmission and the wait for the peer's ACK must be done by the MAC/PHY sequencer, which means one analog + digital state that holds both paths (what the vendor PHY leaves while connected). Needs: capture that state with `bench/idf_phydump` (connected STA, idle) and diff against our RX and TX sets.
2. ACK-enable demonstration: requires a sender that transmits directed frames. The peer's `esp_wifi_80211_tx` (STA or AP interface, data, probe request, even a tagged beacon) never reached the air in this setup (accepted, `ESP_OK`, nothing received while its real AP beacons were); do not use it as a frame source. Use a real association (peer AP) instead: its directed replies repeat with the retry bit until acknowledged.
3. TX status / retry bits, rate field values for 6/54 Mbit/s, hardware CCMP, beacon timer: not probed yet.

## Gate result (plan Task 3 step 5)

(a)/(b)/(f)/(g) unanswered until open item 1 is solved; Wi-Fi tasks (6-12) are re-estimated as a reverse-engineering track. BLE (Phase C) does not depend on any of this and proceeds first.
