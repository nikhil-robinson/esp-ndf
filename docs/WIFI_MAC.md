# Wi-Fi MAC on the ESP32-S3 without the vendor stack - findings

Measured with `components/drivers/rf/test_apps/wifi_mac_spike` (DEVKIT) against the IDF peer (`bench/idf_wl_peer`, BOX-3), 2026-10-27.
Register layout follows the ESP32 MAC (esp32-open-mac, plain ESP32 only) with the S3 base `0x60033000`; offsets confirmed on the S3 are marked.

| What | Where / value | Status |
|---|---|---|
| MAC address filter slot 0 / 1 | `0x60033040/44`, `0x60033048/4C` (addr[0..3], addr[4..5]) | present in the captured vendor state (holds the board MAC) |
| filter mask / ACK-enable | `0x60033060/68` = 0xFFFFFFFF; `0x60033064/6C`, bit 16 = enable (value 0x0001FFFF) | present; hardware ACK works (open and WPA2) as long as 0x600330D8 bit 1 stays clear |
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
| RX modulation | 802.11b and OFDM (6-54 Mbit/s) are received. The earlier "DSSS only" was our software noise filter (word 5 of the receive header = noise floor, set on every OFDM frame); the test is now IDF's `rx_state` (header word 11 bits 31:24, `wifi_pkt_rx_ctrl_t`) | done: AP beacons at 1/6/12/24 Mbit/s all found; an unpinned IDF AP sends us data at 48/54 Mbit/s |
| HT (802.11n) receive | HT Capabilities 0x116e as IDF (0x012c, 20 MHz only, with NDF_WIFI_HT40 0). 40 MHz with the secondary above or below, from the AP's HT Operation element: LO at the 40 MHz centre, SYSCON 0x6002600c bit 2, 0x6001cc0c bit 0 (below) and IDF's HT40 words (`rf_wifi.c`, ROM `rom_bb_bss_cbw40*`). IDF's 802.11n receive registers (same-board 11b/g/n vs 11b/g-only diff, `bench/wifi_regs/data/idf_box3_n_vs_bg.json`) and the per-rate TX digital gain table (`rom_set_tx_dig_gain`, 0x60006014..30) are set at RX start (`rf_wifi.c`). Duplicate cache per TID (sequence + fragment), fed only by our AP's frames to us (another station's frames, heard in 40 MHz mode, made it drop ours). 20 MHz frames in 40 MHz mode leave at the primary (0x60033400 bits 1:0, ROM `rom_mac_tx_chan_offset`); the expected response rate per frame (0x60034314 bits 13:6, `mac_tx_set_plcp1`). | done for 20 MHz: same board (BOX-3) MCS0-6 equal to IDF, 40 MHz received (above and below, MCS0-7, equal to IDF on the same board); the AP sees us at -20 dBm vs IDF -18 dBm (see PERFORMANCE.md). |
| Hardware ACK | works: filter slot 0 = our MAC with enable (0x60033064 bit 16). Earlier failures on protected networks came from the key-table setup writing the OR of bit 1 into `0x600330D8` (IDF's rxq policy bit): that bit stops ACKs (AP frames per 10 pings: 17 without, 224 with) and decryption does not need it | confirmed (bisected with the sniffer frame count; WPA2 ping 19/20, 36 AP frames per 20 pings) |
| TX start cuts off the ACK | `wifi_mac_tx` resets the MAC (TX init); doing that right after a frame to us aborts the hardware ACK still being sent (about 314 us at 1 Mbit/s), so the AP repeats the frame and our reply collides with the repeat. `wifi_mac_tx` waits `WIFI_ACK_US` = 400 us after the last frame to us. Earlier 4 ms wait that drained the ring dropped frames (UDP loss, false beacon timeouts); removed | confirmed: peer pings 4-11/20 -> 19-20/20, AP frames per 20 pings 40 -> 23, 12 UDP bursts x 100 datagrams no link loss |
| WPA2 station | 4-way + group handshake works (join 0.8 s, wrong passphrase -> -3 in 2.8 s, 6/6 joins with EAPOL at 1 Mbit/s). **Receive is done by the MAC's key table (solved 2026-10-29):** see the next rows | done |
| Hardware key table | slot n at `0x60034400 + 0x28*n` (IDF used slots 0 = pairwise, 4 = group): transmitter address (6 B), flag halfword (`0x48cc` pairwise, `0x086c` group; CCMP = 3 in the algorithm field), 16-byte key; `0x60033800` (interface 0 crypto control) goes `0x30000 -> 0x30103`, `0x60033814` has one enable bit per slot. The MAC reset in `rf_wifi_rx_init` clears all of it. Programmed identically, protected unicast frames from the AP still do not arrive | open; dumps in `bench/wifi_regs/data` |
| Key table, tried | pairwise + group slots written persistently (reinstalled after every TX/RX switch), all eight word/byte orders of the key, flag bit flips, IDF's whole interface-0 configuration cloned: no encrypted unicast frame from the AP ever reaches the ring (group frames do). Needed to separate hardware drop from PHY decode: a third radio sniffing the AP while another station talks to it | open |
| WPA2 downlink, isolated (3 radios) | with the BOX-3 sniffing: the IDF AP does transmit the protected unicast replies to our MAC (ARP reply 80 B, ping requests 144 B, 1 Mbit/s, once each, no retries), and a unicast CCMP frame encrypted on the host with our own pairwise key and injected by the AP board is also on the air (verified by the sniffer). Our MAC never delivers either to any ring, with the key table programmed like IDF (slots 0 and 4, state words cleared, `0x60033800 = 0x30103`, `0x60033814 = 0x11`), IDF's interface-0 BSSID/filter settings cloned, any of 8 key byte orders. Group frames and unprotected unicast arrive. So the MAC's crypto stage consumes them; what IDF does differently is still unknown. Next step: watch IDF's writes to the key RAM and crypto registers with a JTAG data watchpoint during an IDF station join | open |
| RX queues | `0x60033088` receives nearly every frame on the air including noise (this is where the junk frames come from), the other heads are other queues; IDF's heads all point into one descriptor pool | noted |
| Hardware CCMP receive | IDF's `hal_crypto_*` (read from the library disassembly in the peer ELF): slot n at `0x60034400 + 0x28*n` = TA (6 B), flag halfword, key at +8, words 6-9 zero; `0x60033814` one enable bit per slot; `0x60033800` = `0x30103`; **`0x60033810` bits 21:6 must be clear** (the replayed RX register set has them set: then protected unicast frames are consumed); do NOT set `0x600330d8` bit 1 (IDF rx queue policy): it stops the hardware ACK, and decryption works without it. Measured with host-encrypted frames injected by the AP and an oracle (the delivered bytes against AES-CCM): slots 0-3 never decrypt CCMP, slots 4-24 do; the highest matching slot wins and the key id (bits 9:8 of the halfword) separates pairwise (0) from group (1..3); flag halfword **0x086c | key id << 8** decrypts and verifies unicast and group frames, while IDF's pairwise word 0x48cc (its transmit key) makes the MAC abort the frame (DMA stops after 28 bytes). The MAC delivers the frame with the CCMP header kept and the MIC removed; frames it cannot verify are dropped | done (`wifi_mac_set_key`) |
| RX descriptor length, final | normal frame: frame + 44 rounded up to 4 (155/156 -> 200, 137 -> 184); MAC-decrypted frame: frame + 36 (MIC removed) | done |
| RX queues | `0x60033088` receives every frame (queue A); queues B/C get none with our filters | noted |
| ACK | sent once `0x600330D8` bit 1 is left clear (see above); the 385-frames-per-20-pings figure was the key-setup side effect | confirmed |
| Peer AP quirk | after several `wifi_stop` / `wifi_ap` cycles the IDF AP's DHCP server stopped answering; `wifi_ap` now restarts it | worked around |

## Open (blocks Wi-Fi STA/AP)

1. **TX/RX turnaround solved in software (~700 us + 1 ms settle); hardware ACK works** . `rf_wifi_tx_init` and `rf_wifi_rx_init` each retune and replay a whole register set (ms); after one TX the receiver is dead until `rf_wifi_rx_init` runs again. A Wi-Fi ACK is due 10 us after a frame, so ACK transmission and the wait for the peer's ACK must be done by the MAC/PHY sequencer, which means one analog + digital state that holds both paths (what the vendor PHY leaves while connected). Needs: capture that state with `bench/idf_phydump` (connected STA, idle) and diff against our RX and TX sets.
2. ACK-enable demonstration: requires a sender that transmits directed frames. The peer's `esp_wifi_80211_tx` (STA or AP interface, data, probe request, even a tagged beacon) never reached the air in this setup (accepted, `ESP_OK`, nothing received while its real AP beacons were); do not use it as a frame source. Use a real association (peer AP) instead: its directed replies repeat with the retry bit until acknowledged.
3. TX status / retry bits, rate field values for 6/54 Mbit/s, hardware CCMP, beacon timer: not probed yet.

## Gate result (plan Task 3 step 5)

(a)/(b)/(f)/(g) unanswered until open item 1 is solved; Wi-Fi tasks (6-12) are re-estimated as a reverse-engineering track. BLE (Phase C) does not depend on any of this and proceeds first.

## IDF transmit engine (read from the peer ELF and the ROM, 2026-10-29)

Why TCP is slow (64 KB echo in ~100 s): our `rf_wifi_tx` starts a frame on TX queue 0 but never engages the MAC's frame-exchange engine, so nothing senses the air (CCA/backoff), nothing waits for the ACK, and nothing retries. A reply of the peer that arrives while we transmit collides, we never learn it, and TCP recovers only by its RTO (1 segment alone: 10 ms; 2 back-to-back segments: one of them stalls ~2 s). Measured: a fixed 8 ms blind wait after each TX halves the stalls but starves the receiver (UDP 70% -> 50%, beacon timeout), so it is not kept.

What IDF does (`bench/wifi_regs/data/idf_hal_tx.dis`, `idf_hal_txq.dis`, `idf_lmac_tx.dis`, `idf_lmacSetTxFrame.dis`, `rom_mac_tx.dis`, `rom_lmacTxDone.dis`, `rom_lmacEndFrameExchange.dis`; register state `tx_param_idf_vs_ndf.json`):
- per queue q: CONF `0x60033d04 - 8q` (`hal_mac_tx_config_edca`: bits 27:24 AIFS, 21:12 CW; `hal_mac_tx_config_timeout`: bits 11:0 = ACK timeout; IDF idle value `a20013ff`), PLCP0 / start `0x60033d08 - 8q` (`mac_tx_set_plcp0`: descriptor offset in 19:0, flag bits 21 and 22, 24..28 bandwidth/GI/HT bits, enable = bits 31:30 via `hal_mac_txq_enable`), PMD block `0x60034320 - 76q` with PLCP1 `0x600342f8`, PTI word `0x600342fc` (`hal_set_tx_pti`: rate/retry fields), HT-SIG `0x60034300`, txop queue word `0x60034314` (`mac_tx_set_txop_q`).
- hardware TX operation list `hwtxop` at `0x6003437c`: entry = descriptor offset (19:0) | (header length + payload length) << 20, 15 entries per txop queue (`hal_mac_fill_hwtxop`, `mac_tx_set_txop_q`).
- CCA control `0x60033c50` bits 31:30 (`hal_mac_tx_set_cca`); queue state `0x60033ca4..cb0` (`hal_mac_get/clr_txq_state`).
- results arrive as events in `0x60033c3c` (decoded in `wDev_ProcessFiq`): bit 7 TX complete -> `lmacPostTxComplete`, bit 19 -> `lmacProcessAllTxTimeout`, bit 8 -> `lmacProcessCollisions`, bits 14/24 receive; ROM `lmacEndFrameExchangeSequence` decides retry / success from the queue state and the retry counters, `lmacRetryTxFrame` (IDF) re-arms the queue with the retry bit.
- `hal_init` interrupt mask `0x60033c34 = 0x19a8f9e0` (ours: bit 1 only).

Experiments with those values on our start path (a null frame to the AP / to nobody / broadcast): with c34 = IDF's mask bit 7 appears on every frame, the same for acknowledged, unacknowledged and broadcast frames; CONF `a20013ff`, PLCP0 flags 21/22, and writing IDF's `cf4/cf8/ca0/d78/d84/3c/438/d2c/d34/d14` values did not change the event bits otherwise (queue state registers stay 0). The engine is therefore not driven by these alone: the hwtxop list, PMD fields and the txop queue word (`mac_tx_set_txop_q`) have to be programmed the way `hal_mac_tx_set_ppdu` + `lmacSetTxFrame` do, then the ISR flow (bit 7 / 19 / 8 + queue state) read. Next step: replay `lmacSetTxFrame` for one frame on the peer through OpenOCD and diff the PMD / hwtxop RAM before and after.


### TX parameter RAM (2026-10-29)

`0x600342e0..0x600343fc` (per-queue parameter block: PLCP words, duration, TX-time record, hwtxop list) powers up with random contents and the MAC patches the transmitted frame from it: the duration field (bytes 2-3) comes from `0x60034318`, and some frames got 8 zero bytes at offset 48-55 (observed on DHCP frames: IP destination and UDP ports zeroed, so the receiver dropped them). Seen by sniffing the station's own DISCOVER (BOX-3 `wifi_watch <mac> all` prints the first 64 bytes). `rf_wifi_tx_init` now zeroes the block before the register replay (the replay sets the entries it needs). Before this, TX correctness depended on what the RAM happened to hold after a power cycle.

### Transmit through the frame-exchange engine: solved (2026-10-29)

Root cause of the slow TCP: we switched the radio to a TX register set for every frame (receiver off, nobody listening for the ACK) and started queue 0 with
the "response expected" field cleared, so the MAC never sensed the air, never waited for an ACK and never retried. IDF transmits from its receive state
and lets the MAC run the exchange. Found by halting IDF over JTAG at `hal_mac_txq_enable` on unicast data frames and diffing every MAC register against
ours at the same instruction (`bench/wifi_regs/data/idf_txq_enable_unicast.json`, `ndf_txq_start_unicast.json`), then reading `lmacProcessTxComplete`.

`rf_wifi_tx_ack` (used by `wifi_mac_tx` for every frame):
- no TX replay, no MAC reset: the radio stays in its receive set (our hardware ACKs keep going out while we send);
- IDF's TX configuration (`components/drivers/rf/wifi_tx_cfg.h`: queue 0 CONF `0x420033ff` with the ACK timeout, event mask, PTI word, timing registers);
- start word `desc | 0xC0600000 | 0x01000000` (bit 24 = a response is expected, only for unicast), duration word `0x60034318` = SIFS + ACK;
- completion exactly as `lmacProcessTxComplete`: `0x60033cb0` bits 3:0 = queues done, queue 0 status `0x60034320` bits 15:12 = result (0 success),
  done bit cleared through `0x60033cac`; no completion within the ACK timeout = no ACK;
- `wifi_mac_tx` retries up to 7 times with the retry bit (IDF's short retry limit);
- the interface crypto bit (`0x60033800` bits 8, 1:0) is cleared while sending: our CCMP is software, the engine would encrypt the frame again.

Measured (DEVKIT station, IDF AP, same air): TCP echo 64 KB 95 s -> 2.3 s open, 15.4 s WPA2; UDP 300/300; ping RTT 5.5 -> 3.1 ms.
Next: hardware CCMP on transmit (IDF's path) instead of software CCMP + the crypto toggle, which is what keeps WPA2 slower than open.

### Hardware CCMP on transmit, receive ring order, TX parameter RAM again (2026-10-29)

- **Hardware CCMP transmit** (IDF's path, JTAG capture `bench/wifi_regs/data/idf_txq_enable_ccmp.json`): IDF writes the protected frame with its CCMP header and the plaintext; the MAC encrypts it with the key-table slot of the receiver. IDF's connected key table has the pairwise key twice: slot 0 flags `0x48cc` (transmit) and slot 4 flags `0x086c` (receive). `wifi_mac_set_key` now installs the transmit twin in slot n (0-3) for every pairwise key; `wifi_mac_tx_hwcrypt` sends header + CCMP header + plaintext + 8 bytes MIC room. Frames we still encrypt in software (AP group frames) clear the interface crypto bit while they go out.
- **Receive ring order**: `rf_wifi_rx` returned the first finished descriptor by index; after a wrap that delivered frames out of order and the CCMP replay check dropped the older ones (TCP stalls of 1-2 s). The ring is now read from a read pointer in arrival order.
- **TX parameter RAM**: the engine path does not run the TX register set, so the zeroing of `0x600342e0..0x600343fc` moved to `rf_wifi_rx_init` (zero, then the parameter words of the TX set). Without it, stale contents (they survive resets) made the AP deauthenticate us right after association.
- **WMM** in the association request (as an IDF station): the AP sends QoS data, one TID per queue.


## Receive block ack (A-MPDU), 2026-11-02

Session registers (IDF `hal_agreement_add_rx_ba` / `_del_rx_ba` / `hal_ba_session_store`, `bench/wifi_regs/data/idf_hal_rx_ba.dis`): 8 blocks at
`0x60033274 - 36 * i`: +0 control (`tid << 12 | 0xc0000005` active, bit 31 set while rewriting), +4 peer address 4..5 (and bits 17:16 = interface), +8 peer
address 0..3, +12 window size << 13 | start sequence, +16 / +20 the received bitmap. With a session the MAC answers the peer's A-MPDU with a block ack itself.
Station: `ba_action` in `sta.c` answers ADDBA (category 3 action 0) with status 0, immediate policy, window 16 (`NDF_WIFI_BA_RX=0` declines with 37) and
DELBA ends the session; the AP only asks when it has A-MPDU TX on and the frame is QoS data with TID < 6 (IDF `ieee80211_encap_esfbuf` -> `ieee80211_ampdu_enable`).
Transmit side, from IDF `ppAssembleAMPDU` / `mac_tx_set_htsig`: bit 22 (+19) of the TX header word 0 marks an aggregate; bits 23:22 of the per-queue words
`0x60034310` / `0x60034314` carry the position of the MPDU in it (1 = single frame). Not built yet.

### HT transmit and A-MPDU transmit (2026-11-02)

**HT transmit works** (`rf_wifi_tx_ack`): rate codes 16..23 (MCS0-7) need PLCP1 (`0x600342f8`) bit 25 and the HT-SIG word (`0x60034300`) =
`MCS | length << 8 | 0x07 << 24` (IDF `mac_tx_set_plcp1` / `mac_tx_set_htsig`, libpp disassembly). Before, every HT rate lost all its ACKs (ping 10 sent, 0 received at 0x10 / 0x13 / 0x17);
now ping 10/10 at MCS0 and MCS7 (avg 12.2 / 6.0 ms against 13.9 ms at 12 Mbit/s legacy, `ip_unit` `rate 0x17`, 10 pings each, IDF AP on channel 6).

**A-MPDU transmit works (2026-11-02), opt-in with `NDF_WIFI_AMPDU_TX=1`.** Found by JTAG-capturing an IDF station's aggregates (`bench/wifi_regs/ocd_capture_ampdu.py`, data `idf_ampdu_tx_*.json`; the MAC state after completion in
`ocd_capture_ba_done.py` / `idf_ampdu_done.json`). The software does not build the A-MPDU: no delimiters, padding or FCS. What the MAC wants (`rf_wifi_agg_prepare`, `rf_wifi_tx_ack` with `RF_TX_AGG`):
- **Queue 2** (IDF's QoS queue): CONF `0x60033d04 - 16` = `0x23002560` (the longer response timeout), PTI `0x600342fc - 152` = `0x00122220`, `0x60034310 - 152` = `(MCS & 7) << 28 | 0x400000 | A-MPDU length` (`mac_tx_set_htsig`).
  Single frames through queue 2 also work. Queue 0 was only tried with the wrong start word (no block ack); not retried with the right one.
- **One descriptor per MPDU**, chained: word 0 = `0xA0000000` (+ `0x40000000` on the last) `| size << 12 | length`, length = frame without FCS + 16, size = length - 4; word 2 = next (0 at the end); word 3 = `0x004c3008` (lone MPDU),
  `0x004c2108` (first of several), `0x00042008` (the others). Buffer = 8-byte header (word 0 = MPDU length with FCS | sequence low byte << 16, word 1 = 0) + the frame without FCS. Bit 29 of word 0 is required (without it the exchange fails).
- **PLCP1** `= length & 0xFFF | rate << 12 | 1 << 25`, **HT-SIG** `= MCS | length << 8 | 0x0f << 24` (aggregation bit; required), both with the on-air A-MPDU length = per MPDU 4 delimiter + MPDU with FCS, padded to 4 (not after the last).
- **Start word** `desc | 0xC2600000 | 0x08000000 (several MPDUs)`: bits 31:30 go, 25 = block-ack response, 22:21 HT. **IDF's captured `0x2c` pattern (bits 19, 18 instead of 22) is its 40 MHz setting; with it a 20 MHz station
  never gets a block ack** (queue status 1). That one bit pattern was the whole difference after all other registers matched; found by a runtime switch sweep, bisecting the matched registers.
- Duration `0x60034318 - 152` = 48 (SIFS + block ack). The block-ack result is read as `hal_mac_tx_get_blockack` does (`0x60034334/328/324 - 152`); the MAC fills peer, start sequence and bitmap itself.
- The TXOP list (`hal_mac_fill_hwtxop`, address `0x6003437c + 60 q - 4 (idx - 1)`) stays empty in IDF's 3-MPDU aggregates: the descriptor chain defines the aggregate.
Results: `tools/wl/test_ba_tx.py` PASS (MCS5, IDF AP on channel 1: 271 aggregates block-acked, 0 MPDUs lost, 3 x 256 KB echo intact). Queue status per exchange (`mac` command, `aggst`): 0 = block ack, 5 = no/partial response (2-3% at MCS3-5,
31% at MCS7 on this bench link, where single frames also retry heavily), 2 = rare. Speed: not faster than single frames (docs/PERFORMANCE.md), hence off by default.
