# Standalone PHY bring-up on ESP32-S3 (no Wi-Fi stack)

Goal: bring up the RF block for esp-ndf (power, clocks, PHY init, RF calibration,
tuning, the MAC dump engine) without `esp_wifi_init()` / `esp_wifi_start()`.

Sources used: ESP-IDF v5.3.5 (`components/esp_phy`, `esp_hw_support`, `soc`),
the S3 blobs `libphy.a` / `librftest.a` / `libpp.a` from that release, and the
S3 ROM ELF (`esp32s3_rev0_rom.elf`, esp-rom-elfs 20240305). Everything marked
**known** below is read from that code; **open** items are the questions for
Espressif at the end.

---

## 0. Before anything else: the blobs are windowed-ABI code

`libphy.a`, `librftest.a` and the ROM PHY routines all use the windowed ABI
(`entry` / `retw` / `call8`). esp-ndf runs CALL0 with `PS.WOE = 0`, and its
window overflow/underflow vectors are not implemented. None of the calls below
work from esp-ndf until it has:

- window overflow/underflow handlers (4/8/12) in both vector tables;
- a CALL0 → windowed call shim: set `PS.WOE = 1` and `PS.CALLINC`, `call8` the
  blob function, spill all windows back (`movsp` or a spill routine), then
  restore `WOE = 0`; the shim saves the CALL0 callee-saved registers a12–a15;
- interrupts may stay enabled while in windowed code: esp-ndf's interrupt
  entry only uses the 16 visible registers and saves what it touches, but
  interrupt handlers must not call into the blobs themselves.

The ROM reaches its PHY helpers through a RAM function table `rom_phyFuns`
(pointer at **0x3FCEF3D4**, inside the ROM's reserved DRAM). libphy repoints
entries to its newer copies (`rom_pbus_xpd_tx_on`, for example, exists in both
ROM and libphy). esp-ndf's RAM map ends at 0x3FCE9000, so it does not
overwrite that table; nothing else has to change for it.

---

## 1. Standalone PHY bring-up

### 1.1 Power domain (known, `esp_wifi_bt_power_domain_on`, S3 path)

```
RTC_CNTL_DIG_PWC_REG  &= ~RTC_CNTL_WIFI_FORCE_PD         // power the Wi-Fi/BT domain
delay 10 us
SYSCON_WIFI_CLK_EN    |= 0x0078078F                      // common clocks on (temporarily)
SYSCON_WIFI_RST_EN    |=  MODEM_RESET_FIELD_WHEN_PU      // pulse modem resets:
SYSCON_WIFI_RST_EN    &= ~MODEM_RESET_FIELD_WHEN_PU      //   WIFIBB, FE, WIFIMAC, BTBB, BTMAC,
                                                         //   RW_BTMAC, RW_BTMAC_REG, BTBB_REG
RTC_CNTL_DIG_ISO_REG  &= ~RTC_CNTL_WIFI_FORCE_ISO        // remove isolation
SYSCON_WIFI_CLK_EN    &= ~0x0078078F                     // common clocks off again
```

`SYSCON_WIFI_CLK_EN` = 0x60026014, `SYSCON_WIFI_RST_EN` = 0x60026018.
Reset bits: WIFIBB 0, FE 1, WIFIMAC 2, BTBB 3, BTMAC 4, SDIO 5, EMAC 7,
MACPWR 8, RW_BTMAC 9, RW_BTLP 10, RW_BTMAC_REG 11, RW_BTLP_REG 12, BTBB_REG 13.

### 1.2 Clocks for PHY init (known, `esp_phy_enable`)

| bits in `SYSCON_WIFI_CLK_EN` | meaning |
|---|---|
| `0x0078078F` | Wi-Fi/BT common clocks (bits 0–3, 7–10, 19–22) |
| `0x00400000` | PHY calibration clock (bit 22) |
| `0x00788F8F` | `PHY_INIT_MODEM_CLOCK_REQUIRED_BITS`: asserted before init |
| bit 5 / 15 | I2C master (analog regs) / RNG |

`esp_phy_enable()` sets the common clocks, then `phy_module_enable()` (the
bit 22 calibration clock), asserts that all of `0x788F8F` are set, initialises,
and drops the calibration clock again at the end (`phy_module_disable()`). The
common clocks stay on while the PHY is in use.

### 1.3 Init and calibration (known)

```c
int register_chipv7_phy(const esp_phy_init_data_t *init_data,
                        esp_phy_calibration_data_t *cal_data,
                        esp_phy_calibration_mode_t mode);
/* returns ESP_CAL_DATA_CHECK_FAIL (1) when cal_data's checksum fails or is outdated */

typedef struct { uint8_t params[128]; } esp_phy_init_data_t;
typedef struct { uint8_t version[4]; uint8_t mac[6]; uint8_t opaque[1894]; } esp_phy_calibration_data_t;
typedef enum { PHY_RF_CAL_PARTIAL = 0, PHY_RF_CAL_NONE = 1, PHY_RF_CAL_FULL = 2 } esp_phy_calibration_mode_t;
```

IDF's sequence: `phy_init_param_set(1)` (S3 is a combo Wi-Fi + BLE PHY), fill
`cal_data->mac` with the factory MAC from eFuse, `register_chipv7_phy(init,
cal, mode)`. With saved data, the mode is `PARTIAL` after power-on, `NONE` after
deep sleep, and `FULL` when there is no valid saved data.

Then periodically: `phy_param_track_tot(wifi_on, bt_on)` every
`ESP_PHY_PLL_TRACK_PERIOD_MS` (default 1000 ms), under the PHY lock. IDF calls
it from an `esp_timer`; esp-ndf can use a software timer at task level, not in
an ISR.

**Init data format.** 128 opaque bytes. IDF stores them framed as
`"PHYINIT\0"` + 128 bytes + `"PHYINIT\0"`. Known fields (from `phy_init_data.c`
and `phy_init_data.h`): bytes 2–15 are TX power limits in 0.25 dBm units (14
entries); byte 125 is the multiple-bin-support flag; byte 126 is the init-data
type. All other bytes are opaque; use IDF's default table verbatim.

**Saving and restoring calibration.** The blob keeps its own copy, so the
caller's buffer can be freed after `register_chipv7_phy`. IDF stores three
items in NVS namespace `phy`:

- `cal_version` (u32): must equal `phy_get_rf_cal_version() & ~BIT(16)`;
- `cal_mac` (6 bytes): must equal the eFuse MAC;
- `cal_data` (blob): the whole `esp_phy_calibration_data_t`.

On mismatch IDF falls back to `FULL`, then saves when the mode was not `NONE`
and the load failed or the blob returned `ESP_CAL_DATA_CHECK_FAIL`. esp-ndf's
NVS is format-compatible, so the same keys work there.

### 1.4 What the MAC dump engine needs (partly open)

`adctrig` (capture) and `dactrig` (playback) in `librftest.a` assume the Wi-Fi
MAC block is clocked and out of reset, which `esp_wifi_start()` normally
guarantees. From the code: the MAC block is at 0x60033000. `adctrig` claims
internal SRAM by writing 4 to `SENSITIVE_INTERNAL_SRAM_USAGE_3` bits [3:0]
(0x600C101C, `INTERNAL_SRAM_MAC_DUMP_USAGE`). `dactrig` fills its sample
buffer at **0x3FCD0000** (64 K words). Which clock and reset bits beyond 1.1
and 1.2 the MAC needs is **open** (Q1).

---

## 2. The PHY functions

Where each lives (IDF 5.3.5 S3 blobs). None has a public prototype: IDF
declares only `register_chipv7_phy`, `phy_get_rf_cal_version`,
`phy_dig_reg_backup` and a few `ant_*`/`phy_*` helpers in `esp_private/phy.h`.

| function | lives in | notes from the code |
|---|---|---|
| `set_chanfreq` | libphy | |
| `set_rf_freq_offset` | libphy | also used by librftest |
| `force_rx_gain` | librftest | writes AGC 0x6001C02C (see 4.1) |
| `set_rx_gain_table` | libphy | touches 0x6001C02C |
| `phy_param` | libphy (data, 0x2E4 = 740 bytes) | layout unpublished; DC-valid flag location **open** (Q3) |
| `chip_v7_set_chan_ana` | libphy | |
| `set_rx_gain_cal_dc` | libphy | |
| `force_tx_gain` | librftest | |
| `force_txon_mode` | librftest | writes FE 0x60006000 |
| `start_tx_tone_step`, `stop_tx_tone` | libphy | write FE 0x60006000 |
| `txcal_debuge_mode`, `txcal_work_mode` | libphy | |
| `rom_pbus_workmode` | ROM | calls `rom_phyFuns[0x19C/4](0)` |
| `rom_pbus_xpd_rx_on` | ROM | pbus writes: (4,1,0), (5,1,0), (0,1,0x184), (1,1,0x189), (1,2,arg) |
| `rom_pbus_xpd_tx_on` | ROM **and** libphy | the libphy copy wins (via `rom_phyFuns`) |
| `rom_pbus_xpd_tx_off` | ROM | pbus writes: (4,1,0), (5,1,0), (1,1,0), (1,2,0), (0,1,0) |
| `rom_set_rxclk_en(bool)` | ROM | 0x60006110 bits 15:14 = en ? 3 : 0 |
| `rom_chip_i2c_readReg`, `rom_chip_i2c_writeReg` | ROM | analog I2C |
| `esp_rom_delay_us` | IDF name for ROM `ets_delay_us` | the blobs import `ets_delay_us` |

Prototypes and semantics for everything except `register_chipv7_phy` are **open** (Q2).

---

## 3. What the blobs need from the OS (known: nm of the S3 blobs against the ROM)

**libphy.a** (the part needed for bring-up, tuning and gain):

| symbol | IDF implementation | esp-ndf |
|---|---|---|
| `phy_enter_critical()` → u32, `phy_exit_critical(u32)` | spinlock; must work from ISRs | `irq_lock` + `xc_spin` |
| `phy_printf(const char *, ...)` | log | printf or no-op |
| `sprintf` | newlib | lib/printf.c |
| `coex_pti_print` | coex lib | no-op |
| `ets_delay_us` | ROM | ROM, or own `delay_us` |
| `phy_i2c_enter_critical`, `phy_i2c_exit_critical` (weak) | `regi2c_enter/exit_critical` | lock shared with esp-ndf's analog-I2C users (ADC, BBPLL) |
| `phy_set_pwdet_power(bool)`, `phy_set_tsens_power(bool)`, `phy_get_tsens_value()` (weak) | SAR/temperature-sensor refcounts | must coordinate with esp-ndf's ADC driver |
| `esp_phy_efuse_get_mac`, `esp_phy_efuse_get_chip_ver_pkg` (weak) | eFuse | `sys_info()` |
| soft-double helpers `__divdf3`, `__subdf3`, `__floatsidf`, `__fixdfsi`, `__divdi3`, `__ashrdi3` | ROM | ROM, or esp-ndf's own (callable, CALL0) |

Not needed by the blobs: malloc, timers and `phy_get_lock`. Those are IDF-side
policy (the caller allocates the calibration buffer; PLL tracking is IDF's
timer).

**librftest.a** adds its factory-test shell, which esp-ndf can stub when it
does not call those paths: `cmd_parse`, `read_cmd`, `UartGetCmdLn`, the
`esp_ble_*`/`le_*`/`rw_le_*`/`bt_testmode_init`/`bt_bb_tx_cca_set` BLE test
hooks, `rtc_wdt_*`/`set_rtc_wdt_stg_*`, `dig_gpio_*`, `adc_check_for_module_by_gpio_out0`,
`internal_vol_test`, `freq_offset_get_pwr`, `tx_pwr_backoff`,
`ate_rxdc_remain_check`, `phy_i2c_check`. The real ones it uses come from ROM:
`spi_flash_read`/`esp_rom_spiflash_write`/`erase_sector`, `gpio_output_set`.

---

## 4. Registers (decoded from the ROM and blob code)

### 4.1 AGC control 0x6001C02C / 0x6001C030 (known)

- 0x6001C02C bits [31:24]: RX gain value used while forced (`rom_rx_gain_force(force, gain)`).
- 0x6001C02C bit 23: force enable.
- 0x6001C030 bit 8: **AGC disable**. `rom_disable_agc` sets it; `rom_enable_agc`
  clears it, then writes gain 0x3E with force on and releases force (seeding the AGC).

So bit 23 alone forces the gain value, but the AGC state machine keeps running
unless 0x6001C030 bit 8 is also set. Part of the answer to "why can a forced
gain be taken over": force without AGC-disable is not exclusive (Q4 asks for
the rest).

### 4.2 Front-end pbus 0x60006104…0x60006124 (known)

The pbus is how digital forces values onto the RF front end, as 9-bit fields
addressed by (row 0–5, column 1–2).

- **0x60006104, command:** bit 0 = pbus force mode on (`rom_pbus_force_mode`);
  bit 1 = write strobe; [4:2] = row; [14:6] = value (9 bits); [16:15] = column.
  A write sets bit 1, waits until 0x60006110 bit 31 clears, then clears bit 1.
- **0x6000610C bit 27:** set when force mode is turned off. While
  `SYSCON_WIFI_BB_CFG` (0x6002600C) bit 1 is set, turning force mode off also
  pulses AGC gain 0x32 with force (1 µs, then bit 23 cleared after 2 µs).
- **0x60006110:** bit 31 = pbus busy; bits 15:14 = RX clock enable (`rom_set_rxclk_en`).
- **Readback words** (from `rom_pbus_rd_addr` / `rom_pbus_rd_shift` jump tables):

| row | column 1 | column 2 |
|---|---|---|
| 0 | 0x60006118 [17:9] | 0x60006118 [17:9] |
| 1 | 0x60006118 [8:0] | 0x6000611C [26:18] |
| 2 | 0x6000611C [17:9] | 0x6000611C [8:0] |
| 3 | 0x60006120 [26:18] | 0x60006120 [17:9] |
| 4 | 0x60006120 [8:0] | 0x60006124 [17:9] |
| 5 | 0x60006124 [8:0] | 0x60006124 [8:0] |

Row 1 is the RX gain row: `rom_pbus_set_rxgain` writes (1,2, packed gain bits)
and (1,1, 0x180 | gain[15:8]). **0x60006118 bits [31:18] are not covered by
any ROM pbus helper, and no code in libphy, librftest or libpp touches
0x60006118 directly**, so their meaning cannot be read from the code (Q5).
0x60006114 is read only by librftest's `rxiq_get_pwr` and `phy_corr_get_pwr`
(power estimation), not as a gain word.

### 4.3 Front-end TX force 0x60006000 (partial)

Written by `force_txon_mode` (librftest), `start_tx_tone_step` / `stop_tx_tone`
/ `phy_txtone_*` (libphy), and ROM `rom_set_tx_dig_gain`, `rom_set_tx_gain_mem`,
`rom_fe_reg_init`, `rom_fe_i2c_reg_renew`. Bit meanings are **open** (Q6).

### 4.4 Dump engine 0x60033D5C / 0x60033D64 (partial, from `adctrig` / `dactrig`)

- **0x60033D64, DAC playback:** bit 31 = start (cleared first); [13:0] = length;
  bit 15 = set unless mode == 1; bit 19 = loop/flag argument; [27:20] = low
  8 bits of the first argument; bit 18 = done (polled). Samples are read from
  0x3FCD0000.
- **0x60033D5C, capture:** bit 31 = start (cleared first). Field write:
  `[31:28] = source, [27:20] = arg, bit 17 = flag, bit 16 = !flag2, [15:0] = length + 1`.
  Bit 19 is set on one path; `dactrig` writes 0 to this register when done.
  Some capture modes (4, 5, 1) first force the AGC gain through 0x6001C02C or a pbus write.

Exact field names and sample formats are **open** (Q7).

---

## 5. Measured on the board (DEVKIT, IDF 5.3.5, bench/idf_phydump)

Register snapshots: before Wi-Fi, after `esp_wifi_init`, after `esp_wifi_start`,
and a second firmware that does only `esp_wifi_bt_power_domain_on()` +
`esp_phy_enable(PHY_MODEM_WIFI)` (no Wi-Fi stack).

- **PHY-only bring-up works** and reaches the same state as `esp_wifi_start()`
  for everything the PHY owns:
  - `SYSCON_WIFI_RST_EN` = 0x3E08 (only BT blocks held in reset);
  - `SYSCON_WIFI_BB_CFG` = 0x10000000 and `WIFI_BB_CFG_2` = 0x000F000B
    (both set by PHY init, not by the MAC);
  - `RTC_CNTL_DIG_PWC.WIFI_FORCE_PD` = 0, `DIG_ISO.WIFI_FORCE_ISO` = 0,
    `LOW_POWER_ST.XPD_WIFI` = 1, `OPTIONS0.BB_I2C_FORCE_PU` = 1.
- **What the MAC init (libpp) adds on top:**
  - `SYSCON_WIFI_CLK_EN` trimmed from the reset value 0xFFFFDFDF (everything
    on) to 0xFF87D850;
  - `RTC_CNTL_ANA_CONF` force-PU bits for PLL/CKGEN/RF-pbus/TX-RF I2C cleared
    (power-save handover);
  - 74 MAC words at 0x60033000+: the station MAC address (0x60033000/40/48),
    filters, timings and queues;
  - 31 FE/AGC/NRX/BB words, consistent with the radio held in RX by the MAC.
- **The dump-engine control words read 0 in both runs** (0x60033D5C,
  0x60033D64; status at 0x60033D60). The MAC block is clocked and readable
  after PHY-only bring-up, so the dump engine needs nothing from `esp_wifi_*`
  that this snapshot can see. To be confirmed by a capture (below).
- **`adctrig` signature** (librftest, windowed, 9 arguments):
  `adctrig(len, src, f20, f16n, f17, gain_mode, gain_a, gain_b, settle_us)`.
  Capture word = `src << 28 | f20 << 20 | f17 << 17 | (!f16n) << 16 | (len + 1)`.
  `gain_mode` 1/3/4: AGC force through 0x6001C02C; 5: pbus row write.
  Samples go to **0x3FCD0000** (internal SRAM; `SENSITIVE_INTERNAL_SRAM_USAGE_3 = 4`).

Next measurements (esp-ndf side, where 0x3FCD0000 can be reserved):
capture after PHY-only bring-up; vary one capture field at a time; `phy_param`
before/after `set_rx_gain_cal_dc`; `--wrap` call logs of the PHY functions
under the full Wi-Fi stack for their prototypes.

## 6. Questions we answer by measurement (formerly "for Espressif")

1. **MAC/dump-engine enables.** Besides the common clocks (0x78078F), the
   calibration clock (bit 22) and the reset release in 1.1, which
   `SYSCON_WIFI_CLK_EN` and `SYSCON_WIFI_RST_EN` bits, and which MAC-side
   registers, must be set for the dump engine (0x60033D5C capture,
   0x60033D64 playback) to run without `esp_wifi_start()`? Is anything from
   libpp's `mac_init` required?
2. **Stable prototypes and semantics** for `set_chanfreq`, `set_rf_freq_offset`,
   `force_rx_gain`, `set_rx_gain_table`, `chip_v7_set_chan_ana`,
   `set_rx_gain_cal_dc`, `force_tx_gain`, `force_txon_mode`,
   `start_tx_tone_step`, `stop_tx_tone`, `txcal_debuge_mode`,
   `txcal_work_mode`, and the ROM `rom_pbus_*` / `rom_set_rxclk_en` /
   `rom_chip_i2c_*` routines. Which of them are stable across IDF/PHY versions?
3. **`phy_param` layout** (740 bytes in libphy for S3): the offset and meaning
   of the DC-valid flag, and which fields `set_rx_gain_cal_dc` reads and writes.
4. **Forced gain takeover.** With 0x6001C02C bit 23 set and 0x6001C030 bit 8
   set (AGC disabled), what can still override the forced gain: pbus force mode
   off (0x60006104 bit 0), PLL tracking (`phy_param_track_tot`), or MAC TX/RX
   state changes? What is the guaranteed way to hold a gain?
5. **0x60006118 bits [31:18].** Bits [17:0] hold pbus readback for row 1/col 1
   and row 0. What are bits [26:18] and [31:27]?
6. **0x60006000 (FE TX force)**: bit map used by `force_txon_mode` and the tone functions.
7. **Dump engine field definitions** for 0x60033D5C / 0x60033D64, the sample
   format in SRAM, and the meaning of the value 4 in
   `SENSITIVE_INTERNAL_SRAM_USAGE_3[3:0]` (which SRAM bank, and how to give it back).
8. Are `register_chipv7_phy` and `phy_param_track_tot` safe to call with only
   the hooks in section 3 (no FreeRTOS, no `esp_timer`), and must calls be
   serialised against interrupts beyond `phy_enter_critical`?

---

## 7. Blob-free radio (drivers/rf): measured answers

Our own code, no vendor PHY code linked (checked in the map file), on board
C9:48. Runs the same on IDF (`bench/idf_rfrx OURS=1`) and on esp-ndf
(`examples/rf_test`).

- **Init:** `rf_init_replay()` is the vendor init as data:
  - The ordered ops are generated by `bench/idf_rftrace/gen_init.py`: 402 analog-bus writes, 1293 pbus writes, 314 gain-memory writes and 28 pbus force-mode changes.
  - It also writes 84 digital words, before and after the ops.
  - The ops carry this chip's calibration results (`rf_init_replay_c948.h`, `rf_txcal_c948.h`).
  - Our own calibrations replace them one group at a time.
- **Receive:** `rf_pll_set_khz()` (section 5, bit-exact), then `rf_rx_on()` and `rf_rx_gain()`. Capture is `rf_dump_burst()`.

| Gain index | 0–30 | 40 | 60 | 70 | 80 | 90–120 |
|---|---|---|---|---|---|---|
| RMS, vendor init on IDF | 1.2–1.5 | 2–4 | 4–20 | 11–147* | 33–100* | 1.2–1.6 |
| RMS, ours on IDF | 1.1–1.4 | 5 | 4 | 10 | 35 | 1.1–2 |
| RMS, ours on esp-ndf | 1.2–1.7 | 2 | 3 | 10 | 31 | 1.2–1.5 |

\* Wi-Fi traffic on the channel: these values vary capture to capture.

### Answers

- **Q1, dump-engine enables.** Three conditions, found by bisecting full Wi-Fi state against PHY-only state:
  1. **0x6002600C bit 1** must be set. `esp_wifi_start` sets it and `esp_wifi_stop` clears it. Without it, the engine stores one frozen sample: every word is identical, though the gain field still updates.
  2. **0x60006110 bit 24** must be set when **0x6001C04C is written** (any value). After that the ADC stream runs. Clearing bit 24 afterwards does not stop it. The vendor init sets bit 24 and later writes 0x6001C04C.
  3. **SYSCON_WIFI_CLK_EN** needs the full IDF mask **0x00FB9FCF**. 0x78078F is not enough: with bits 6/12/16/17 gated (esp-ndf's boot default), the engine never writes SRAM.

  No MAC register is needed. Neither is libpp.
- **Q4, forced gain takeover.** 0x6001C02C = gain << 24 | bit 23 holds the gain. The takeover comes from `rom_pbus_force_mode(0)`, which `rom_pbus_workmode` calls:
  - when 0x6002600C bit 1 is set, it writes gain 0x32 with force on;
  - it waits 2 µs;
  - then it clears force (bit 23), handing the gain back to the AGC.

  So the forced gain must be applied after any workmode call.
- **Q6, 0x60006000 bits:**
  - bit 1: TX forced on;
  - bits 17:10: forced TX gain row;
  - bits 25:18: TX gain-row base (Wi-Fi rows base..base+13; BT rows base+16..base+31);
  - bit 26: tone generator path off (`stop_tx_tone` sets it).
  - `rom_force_txon(1)` also sets 0x60006110 bits 9:8 = 2 and bits 11:10 = 3.
- **Q7, dump engine fields:**
  - **Capture (0x60033D5C):** bit 31 run, bit 19 start pulse, bit 18 done, bit 16 = 16 MS/s, bit 15 = 40 MS/s (neither = 80), bits 13:0 count. Configuration 0x60033D90 = 0x000C2040.
  - **Capture word:**
    - I in bits 9:0 and Q in bits 19:10 (signed 10-bit);
    - gain index / 2 in bits 27:21;
    - bit 29 set while the stream is live;
    - bit 31 marks some words.
  - **Playback (0x60033D64):** bit 31 run, bit 19 trigger (held = gapless loop), bit 18 done, bit 15 = 80 MS/s (else 40), bits 13:0 count.
  - **Bank select 0x600C101C [3:0]:** one-hot bank owner, 0 = CPU. Banks 0–2 are at 0x3FCB0000 + 64 KB·n; bank 3 holds ROM data.
  - **Playback and capture together:** they share one address counter. While the DAC plays n words from bank 1, a capture writes exactly n words into bank 2, sample for sample, whatever its own count.
  - The CPU reads garbage from a bank the engine owns.
  - With TX forced on (0x60006110 bits 11:8), the receive samples freeze (constant value, bit 29 still set).
  - In loopback with the TX force released, the stream stays live and its DC moves with the PA step (I −10 → −40 for PA 0 → 10): TX LO leakage. The TX gain rows are not applied in that state.
- **TX gain memory** (`rom_set_tx_gain_mem`). Each of the 14 Wi-Fi rows (and the 16 BT rows) is `write_gain_mem(lo, hi, base + i)` with:
  - `lo = w0<<12 | w1<<3 | w2>>6 | cal2<<21`
  - `hi = w2<<26 | w3<<17 | gain<<10 | (bb&63)<<3 | bb>>6`
  - `w[4]` is a per-chip TX-cal row, chosen by bb class: 128→1, 256→2, 32→3, 160→4, else 0 (BT: 128→1, 256→2, else 0).
  - The tables are at `phy_param` +0x124/+0x14C (Wi-Fi) and +0x182/+0x180 (BT).
  - Digital TX gain bytes are at 0x60006014..0x60006030.
- **Q8:** not needed. No vendor code runs.

**Transmit (`rf_tx_on`, `rf_dac_play`):** after TX-on, the whole FE block 0x60006000..0x600061FC (including the pbus readbacks at 0x60006118..24) equals the vendor's, except the live 0x6000607C.

**Over the air (`examples/rf_ota`, two boards about 10 cm apart, 2437 MHz).** D0 sends a +2 MHz DAC tone, 1 s on / 1 s off, at gain 127; C9:48 receives at forced gain 20.

| Transmitter | Receiver | Tone on | Tone off | Image |
|---|---|---|---|---|
| vendor (IDF) | ours (esp-ndf) | +36 dB | about −32 dB | about −10 dB |
| ours (IDF) | ours (esp-ndf) | +37 dB | about −33 dB | about +3 dB |
| ours (esp-ndf) | ours (esp-ndf) | **+36.6 dB** | **−31.3 dB** | +2.7 dB |

- Both directions work with no vendor code.
- The image sits about 34 dB below the tone with our TX, against about 46 dB with the vendor's, as expected without our own TX IQ calibration.
- A forced RX gain left on (0x6001C02C bit 23) blocks transmit, so `rf_tx_on` releases it.
- The received spectrum is mirrored: a +2 MHz transmitted tone lands at −2 MHz, so raw capture Q is inverted (esp_sdr negates it too).
- **Cold boot:** RTC_CNTL_ANA_CONF (0x60008034) bits 27/28/30/31 (TXRF, RFRX-pbus, CKGEN, PLL analog-bus power) must be set, or the PLL calibration never completes. They survive a chip reset, which hid this while vendor runs preceded ours.

**Correction:** an earlier "±2 MHz loopback of about 0.3 LSB" was an artefact. Those captures were mostly unwritten words: the capture length follows the DAC count.

**Internal loopback (`rf_loopback`, from `rom_loopback_mode_en` / `rom_set_loopback_gain`):**
- analog 67.00[6] on;
- TX and RX clocks on (0x60006110 bits 17:16, 15:14);
- pbus (4,1,127), (5,1,pa), (0,1,0x104), (1,1,0x1F9), (1,2,rxg).

**Baseband estimator (`rf_bb_est`, from `rom_iq_est_enable` / `rom_dc_iq_est`):**
- **Start:** 0x60006140 bits 27:26 = 01; in 0x60006144, bits 20:19 = 10, n in [16:2], bit 0, then bit 1.
- **Done:** poll 0x60006174 bit 16.
- **Results:** ΣI, ΣQ, power at 0x6000615C / 60 / 64, each >>6 and /(n+1).
- It agrees with the capture's DC, with I and Q swapped and sign-inverted relative to the capture word fields.
- With TX forced on in loopback, it still reads only the receive-side DC: the vendor's TX-cal sequence (`txcal_debuge_mode`, tone generator) is needed to get TX into the loopback. That is the next step for TX DC/IQ calibration.

**Streaming (`rf_stream`, drivers/rf/rf_stream.c).** Circular mode (0x60033D5C = 0x24000 | rate | run) writes at the ring index 0x60033D60 (mod 16384) into the selected bank:
- Switching banks every 12288 pairs splits the stream into units.
- Sentinel windows locate each unit, and unit n+1 must start exactly where unit n ended (gapless by construction).
- A consumer that laps the ring is caught as a gap.

Measured on esp-ndf, interrupts masked, single core:

| Rate | Units | Samples | Measured rate | Max lateness | Consumer |
|---|---|---|---|---|---|
| 80 MS/s | 1000 | 12.30 M | 79.87 MS/s | 7 pairs | light |
| 40 MS/s | 300 | 3.69 M | 39.81 MS/s | 3 pairs | sums every I sample (3.5 cycles/pair) |
| 16 MS/s | 300 | 3.69 M | 15.97 MS/s | 1 pair | sums every I sample (3.5 cycles/pair) |

The measured rates include the start. The consumer's budget per unit is about 12000 pairs, which is about 2.4 cycles per pair at 80 MS/s, so heavier work belongs on core 1.

**Calibration state across boots** (two vendor init traces, same chip):
- All 54 analog registers and all 10 pbus rows end identical.
- Only gain memory varies:
  - RX rows 0..159 carry the per-gain DCO codes, from `pbus_rx_dco_cal` (DCO search on pbus rows (2,1)/(3,1), measured with the BB estimator 0x6000615C..64);
  - TX rows 160..191 carry the TX DC values.
- Chip-specific (but boot-stable) analog values come from the init's searches: PLL cap, block 0x6B TX caps against the SAR power detector, block 0x67.

**Still open:**
- **Second chip (D0) with C9:48's replay plus our RX DC calibration:** PLL locked; RX DC within ±2.4 LSB up to gain 60; transmits.
- **TX DC (LO leakage) calibration, done, our own: `rf_tx_dc_cal()`.**
  - The vendor measures leakage with the TX power detector through SAR2, not with an IQ estimator:
    - `en_pwdet` + `pwdet_sar2_init`: RTC 0x60008830/34, 0x6000E050/5C/60, 0x60006048 bit 7;
    - `pkdet_vol_start`: 0x6000E05C bits 23/19, pulse 0x6000E050 bit 1, wait for [26:24] == 7;
    - results: 8 × 13 bits at 0x6000E080..9C.
  - Transmitting with no DAC data, that reading is a smooth V in each TX-row DC word (w0 = lo[20:12], w2 = hi[31:26]+lo[2:0]); `rf_tx_dc_cal` searches it coarse to fine.
  - Over the air on D0 (receiver offset 1 MHz so its own DC is out of the way; tone +36 dB):

    | D0 TX DC words | LO leakage |
    |---|---|
    | copied from C9:48 | −21 dBc |
    | neutral 0x100 | −31 dBc |
    | **ours, from neutral, 28 ms** | **−33 dBc** |

- **TX IQ (image):** the correction is 0x6000607C [10:0], gain [4:0] (5-bit signed) and phase [10:5] (6-bit signed), the same layout as the TX rows' IQ word lo[31:21] (libphy `txiq_set_reg`).
  - Over the air on D0, the image is best near gain 0 / phase 0..+8 (−42 to −43 dBc) and −22 to −30 dBc elsewhere. The stale copied word gave −34 dBc; the vendor's own calibration gives −46.
  - **Default is now the neutral word:** −41 to −45 dBc measured.
  - **Self-calibration: closed, not adopted.** The vendor measures with two tone-generator test modes (0x60006040 bits 27:24) through the power detector (`txiq_get_mis_pwr`, `txtone_linear_pwr`, `pwdet_tone_start`, baseline at `phy_param`+218).
    - Calling the vendor's own `txiq_get_mis_pwr` on D0 (`bench/idf_rfrx IQPROBE=1`) across gain −12..+12 and phase −24..+24 gives readings of about 5000–6600, with mode differences a constant 30–100 counts plus drift. It barely responds, although the same settings move the over-the-air image from −43 to −21 dBc.
    - The method is too coarse to beat the neutral word: −41 to −45 dBc against −46 for the vendor's word, and 0 needs no data. Neutral it is.
- **Chip-specific analog values (closed).** I diffed the vendor init of C9:48 against D0 (`bench/*/data/*_c9d0.*`). Only these differ:
  - **62.01/02:** PLL cap, computed by `rf_pll_set_khz`.
  - **6B.02:** per-channel TX cap, ±1.
  - **67.14..1F:** RX filter, every register ±1. It comes from an RC calibration: configure 6A, pulse 6A.04[3], read 6A.05[5:0]. The code is 0x23 on C9:48 and 0x24 on D0, and the vendor adds it to a fixed table. `rf_rc_cal()` (inside `rf_init_replay`) measures it with a 100 µs settle and an average of 16 reads (it dithers ±1), and reproduces both chips' values exactly.
  - **69.03/04:** the SAR2 init code, a constant (0x578), skipped when already set.
  - **69.06:** read and rewritten with bit 3 flipped on every init, so not chip data.
- **Noise at gains 50–80 on C9:48 (closed):** it was D0's LO nearby. With D0's radio off, C9:48 shows the vendor's profile (RMS 2–33).
- ~~LO divider~~ done: `rf_tune_rx_khz()` runs the PLL at 6/5 of the LO with 65.00[4] set below 2240 MHz. The VCO locks directly from about 2230 to 2760 MHz (`examples/rf_range`), so receive covers about 1.85–2.76 GHz and transmit about 2.23–2.76 GHz. Checked over the air: receiver on the 6/5 path at 2280 MHz (PLL 2736), transmitter direct at 2280: tone +43.9 dB on, −32.1 dB off. Sub-GHz is out of reach: the divider register cannot go below 960 MHz, and the front end is matched for 2.4 GHz.
- TX checked over the air.

## 8. Open-list progress (A/B/C)

- **A1, retune:** `rf_pll_set_khz` (hardware cap calibration + refinement) takes 390 µs. `rf_pll_hop(khz, cap)` with a cap cached from an earlier tune is 8 analog-bus operations, 38 µs (3.44 µs per bus write); the vendor measured 0.7–0.9 ms. LO settling after a hop over the air: pending (needs board D0).
- **A1, analog-bus map:** `docs/RF_REGMAP.md`, generated from both chips' vendor traces by `bench/idf_rftrace/regmap.py`: 56 registers in 8 blocks, final values per chip, chip-specific ones marked, meanings where we established them.
- **A4, rates (`examples/rf_dumpinfo`):**
  - capture control bits 16:15 give 80, 40 or 16 MS/s (measured 79.75 / 39.94 / 15.99 including start-up); bit 17 is circular mode; bits 23:20 do not change the rate;
  - there is no hardware decimator below 16 MS/s, so lower rates mean software decimation of a stream.
  - The config register 0x60033D90 selects the source of each byte lane of the stored word (0 gives four copies of one byte), so 8-bit packing is possible.
  - Config bit 24 stores one word per baseband event (64 words took 0.2–33 ms, varying): an event-gated capture, a lead for the energy/packet detector (A2).
- **A4, playback position:** no register follows the 40 MS/s play index. 0x60033D58 and 0x60033D80[9:0] change during playback but much more slowly.
- **A6, clocks:** over 1 s of SYSTIMER, the ring write index runs at 80.000003 MS/s (40.000001 at 40), and the CPU at 240.000010 MHz. All derive from the 40 MHz crystal, so the write index is a sample-exact timer with no drift against the CPU. 0x60033D4C counts about 170 k events per second (not a clock).

- **Fixed: transmit from a true power-on.** The vendor's `bb_init` → `set_pbus_mem` fills a **pbus memory**: per-state front-end command lists (46 words) that the hardware applies on its own at TX/RX transitions.
  - Port: the gain-memory port at addresses 0x200.. (data 0x600060CC, address 0x600060C8[17:8]), with range fields 0x600060E0..F4.
  - It is write-only, so no register dump shows it, and it survives every reset except power-on. That hid it, and our TX was silent from cold.
  - `tools/gen_pbus_mem.py` emulates the function from its disassembly and read-only tables; the range fields it computes match the hardware exactly. Our init now loads it (`pbus_mem_load`, `drivers/rf/rf_pbus_mem.h`).
  - Result from power-on (D0 on its UART port, every run cold): TX +41 to +45 dB, 3/3. That is about 10 dB more than our earlier output, which had lacked these lists all along.
  - How it was found:
    1. Vendor init + our TX worked; our init + our TX did not.
    2. Layering our replay on the vendor init broke nothing (retune included), so the gap was state the vendor creates that the replay never touches.
    3. Vendor `rf_init` + `bb_init` after our init restored TX; `rf_init` alone did not.
    4. Of `bb_init`'s callees, `set_pbus_mem` writes write-only memory.

## 9. Measurements (A.1 to A.3), two boards about 10 cm apart

All levels are relative dB at the receiving board (no calibrated power meter or generator). Raw data is in `bench/rfmeas/`, taken with `examples/rfctl` (a line-command radio) and the scripts in `tools/` (`rfhost.py`, `meas_*.py`).

- **A.1, retune:** `rf_pll_hop` with a cached cap takes 43 µs (10,357 cycles), and the received tone is at full level in the very first 3.2 µs block after it (3/3, also from 2240 and 2700 MHz). A full `rf_pll_set_khz` with calibration takes 350–390 µs. `hop_settling.txt`.
- **A.2, receive gain per index** (`rx_gain.csv`):
  - Rows 0..82 and 83..127 are two tables. Each rises about 0.7 dB per index, with resets where the stage mix changes (e.g. 28 → 32).
  - Noise floor 1.3–2.6 LSB RMS up to index ~56, then rising.
  - The CSV holds every row's raw words for decoding the stages.
- **A.2, RX IQ:** captured samples are raw ADC data. The receiver image of a tone is below −45 dBc (at the noise floor), and the baseband RX IQ correction (0x6000607C [20:16] gain, [26:21] phase; libphy `rxiq_set_reg`) does not act on the capture. Capture needs no RX IQ correction.
- **A.2, bandwidth code** 67.04 / 67.05 (I/Q), `rx_bandwidth.csv`:

  | Code | −3 dB corner |
  |---|---|
  | 0x00–0x1C | ≥ 11 MHz (limited by the 40 MS/s DAC test signal) |
  | 0x20–0x24 | 10 MHz |
  | 0x28–0x2C | 9 MHz |
  | 0x30–0x34 | 8 MHz |
  | 0x38–0x3F | 7 MHz |

  At the default 0x1F, the response is −24 dB at 12 MHz and −43 dB at 14 MHz. The other block-0x67 registers barely change the response.
- **A.2, energy detector:** the baseband estimator (`rf_bb_est`, 0x6000615C..64) power output tracks signal level monotonically (0 / 64 / 72 / 209 / 874 for TX off and DAC amplitude 4 / 16 / 64 / 256).
  - 0x60033D4C is an event counter that depends on receive gain (1 M/s at gain 0, 3 k/s at 40), not on signal presence.
  - Config bit 24 captures one word per such event.
- **A.2, compression** (`rx_compression.csv`): at gains 0..30 the received tone follows the input 3 dB per step up to the transmitter's own limit; at gains 40..50 the limit is ADC clipping. The analog front end never compressed first at this distance.
- **A.2, maximum safe input:** not testable without risking the part. The datasheet (`docs/ref/esp32-s3_datasheet_en.pdf`, Table 6-5 "Maximum RX Level") gives +5 dBm typical for 802.11b / 6 Mbps / MCS0 and 0 dBm for 54 Mbps / MCS7. Table 5-1 has no RF damage rating.
- **A.3, TX power** (`tx_power_dac.csv`, `tx_power_dig.csv`, `tx_power_freq.csv`):
  - In DAC-driven forced-TX mode, the TX gain index and baseband code do nothing, the vendor's own `force_tx_gain` ladder included (verified on IDF, `LADDER=1`).
  - Output power is set by:
    - the DAC amplitude: linear at 3 dB per step from 2 to ~128 (about 31 dB), compressing above that, best around 256;
    - the digital gain bytes 0x60006014..30: a signed attenuator, 0x80 = −25 dB to 0xF0 = 0 dB, about 0.2 dB per step; positive values cost about 2 dB.
  - pbus-memory word 19 (0x0044FFFF) is the TX state's PA enable: only values with bits 7:4 all set transmit.
  - Combined TX + RX response vs frequency: peak around 2380–2420 MHz, −10 dB at about 2320 / 2520 MHz, −35 dB at 2240 / 2700 MHz.
- **Note:** two sessions came up with no link out of about 30 (cause unknown: TX or RX side). `tools/rfhost.link()` checks the level and re-initialises, logging each case.
- **A.4, where the samples can go** (`examples/rf_dma`):
  - The dump engine writes only SRAM banks 0..2 (0x600C101C[3:0]; bank 3 holds ROM data). It has no descriptor interface and cannot target PSRAM.
  - Moving the stream out:
    - GDMA memory-to-memory runs at 63 MB/s, so async GDMA per unit keeps up only at 16 MS/s (2/400 copies late);
    - a CPU `memcpy` runs at 1885 MB/s: 27 µs per 48 KiB unit, so 80, 40 and 16 MS/s all stream gaplessly with every unit copied (1000 units each, lateness ≤ 7 pairs).
  - PSRAM: esp-ndf has no PSRAM driver yet. Even an octal PSRAM (~80–160 MB/s) cannot take 80 MS/s × 4 B (320 MB/s). 16 MS/s (64 MB/s), or 8-bit lanes through the config register (half that), fit octal PSRAM; quad PSRAM (~40 MB/s) fits ≤ 8 MS/s.
  - Lower rates than 16 MS/s: software decimation of the stream (no hardware decimator).

## 10. A.5 to A.8, B and C

### A.6 Timing
- Capture starts 45 CPU cycles (188 ns) after the run bit (`rfctl lat`).
- The sample clock is 80.000003 MS/s against SYSTIMER. Both come from the 40 MHz crystal, so a sample index is a drift-free timestamp.

### A.7 Crystal and temperature
**Temperature sensor driver** (`drivers/tsens`, `include/tsens.h`):
- Registers: SENS 0x60008850 / 54, clock gate 0x60008904 bit 29.
- The range DAC is analog register 69.06[3:0]. This explains why 69.06 toggled between inits.
- Conversion: T = 0.4386·raw − 27.88·offset − 20.52.
- Range offset 0 (DAC 15) saturates near 91 °C, so `tsens_read_c` switches to DAC 7 above 75 °C.
- After a range change, the first conversion is stale and must be discarded.

**Crystal offset** (D0 against C9:48, `bench/rfmeas/xtal_vs_temp.csv`, `tools/meas_xtal.py`):
- The tone frequency comes from the phase slope (`rfctl freq`).
- D0 reads −2.30 ppm at 84 °C and −1.58 ppm at 113 °C while it transmits, about +0.025 ppm/°C.
- Trim: offsetting the tune by the measured error leaves about 0.7 kHz at 2.4 GHz.

### C. Characterisation
All values are relative, at about 10 cm, with no calibrated instruments. Raw data is in `bench/rfmeas/c_*.csv`, from `tools/meas_c.py`. Hann window, N 16383, RBW 7.3 kHz.

**LO skirt** (TX and RX LOs combined, so an upper bound on either):

| Offset | Level |
|---|---|
| 20 kHz | ≤ −88 dBc/Hz |
| 50 kHz | ≤ −104 dBc/Hz |
| 100–500 kHz | about −95 dBc/Hz |

At ±1 MHz the bins hold the LO leak and the image (see spurs).

**TX spurs** (DAC tone at +2 MHz, no TX DC calibration in this run):

| Spur | Level |
|---|---|
| LO leak | −22 dBc |
| Image | −40 dBc |
| 2nd harmonic (+4 MHz) | −22 dBc |
| 3rd harmonic (−6 MHz) | −54 dBc |
| +6 MHz | −41 dBc |

The DAC SFDR is therefore about 22 dB, set by the 2nd harmonic at amplitude 300. Run `rf_tx_dc_cal` to remove the LO leak.

**ADC:** the noise is 1.2–2.7 LSB RMS against a complex full scale of 512. That gives SNR 46–53 dB, a noise-limited ENOB of 7.3–8.5 bits. This includes front-end noise, so it is a lower bound on the converter.

**PLL lock range:** 2225–2775 MHz on both chips. Cap 369/375 at the bottom, 2 at the top (`c_lock_range.csv`). Below that range, receive goes through the 6/5 divider (`RF_LO_DIRECT_MIN_KHZ`).

**Not measured without instruments:** absolute noise figure, sensitivity in dBm, and IIP3. The datasheet values apply.

**Across temperature:** the crystal is covered above; lock range was measured at about 75 °C only.

### A.5, A.8, B: status
- **A.5, BLE raw mode: works, as a software modem on our API** (`examples/rfctl`: `bleadv`, `blerx`). It interoperates with the vendor BLE stack in both directions.
  - **TX:** the full packet is built in software: preamble, access address, ADV_NONCONN_IND PDU, CRC-24, whitening. It is GFSK-modulated (BT 0.5, ±250 kHz, 40 samples per bit) on a +2 MHz IF and looped from one DAC bank, about 2500 packets/s. An IDF Bluedroid scanner (`bench/idf_blescan`) receives it on channels 37, 38 and 39 at 750–850 packets/s each, at −22 dBm RSSI. The stack drops CRC failures, so the CRC is right (`bench/rfmeas/ble_tx.txt`).
  - **RX:** 1 ms bursts at 16 MS/s. Bursts without energy are skipped. Then an FM discriminator (conjugate product, IF removed, int8), integrate-and-dump per bit, access-address search, dewhitening and the CRC check. Against an IDF advertiser (`ADV=1 idf.py -B build_adv`, channel 37, 20 ms) it decoded 20/20 with the CRC passing, within 0–45 bursts each (`bench/rfmeas/ble_rx.txt`).
  - The capture's Q is inverted versus the DAC (as in `meas`). A signal at +1.83 MHz confirmed the sign.
  - **Hardware BLE receive also works** without the vendor stack: see §11.
- **A.8, coexistence with native Wi-Fi: works** (`bench/idf_coex`, `result.txt`). On one chip, three cycles of Wi-Fi start + scan → `esp_wifi_stop` → our init + receive of D0's tone → Wi-Fi again:
  - every scan found 8–9 APs at −45 dBm, and every receive got the tone at +36.5 dB;
  - hand-off costs: `esp_wifi_stop` 1.6 ms, our init + tune 5.2 ms, the next `esp_wifi_start` 2.3 ms (the vendor keeps its calibration);
  - fix needed: `esp_wifi_stop` (the vendor's RF shutdown) clears analog register 67.02 bit 5, a receive enable that is 1 from power-on and that our replay never wrote. Without it the capture freezes. `rf_init_replay` now sets it. Found by diffing every modem, RTC and analog register between a working and a frozen receive.
- **B:**
  - Prototypes: `drivers/rf/rf_init.h`, `rf_pll.h`, `rf_dump.h`.
  - Calibrations are separate calls: `rf_rc_cal`, `rf_rx_dc_cal`, `rf_tx_dc_cal`, and the PLL cap calibration in `rf_pll_set_khz`. RX IQ is not needed (§9); TX IQ is neutral at 0.
  - OS hooks: only a microsecond delay.
  - ABI: esp-ndf builds everything with `-mabi=call0` (Makefile), so every measurement in this document ran on CALL0 code. The same sources also run under IDF's windowed ABI (`bench/idf_rfrx`, `bench/idf_coex`).

## 11. Hardware BLE receive on the S3's own link-layer core (no vendor code)

`drivers/ble/ble_hw.c`, `include/ble_hw.h`, `examples/ble_scan`. The chip's link-layer core receives advertising PDUs by itself: demodulation, access-address match, CRC, dewhitening, and the write into memory. The CPU only re-arms one scan event per received packet and copies the PDU out.

**Measured** (`bench/rfmeas/ble_hw_scan.txt`; D0 sending about 2,500 ADV_NONCONN_IND/s on channel 37 with `rfctl bleadv`):

| Metric | Value |
|---|---|
| PDUs received | about 1,190/s (935 with half-slot starts) |
| Scan events | about 1,260/s |
| Drops / watchdog re-arms | 0 / 0 |
| CPU | 2.6% in the interrupt handler (about 21 µs per event); the rest is spent asleep in `waiti` |
| `ble_hw_init` | 2.2 ms |

For comparison:
- The vendor scanner (IDF Bluedroid, `bench/idf_blescan`) receives 750–850/s per channel while it rotates through 37/38/39.
- Our software receiver (§10, A.5) needs a whole core while it decodes a 1 ms burst, and cannot run continuously.

**The hardware**:
- **Link-layer core:** RivieraWaves BLE v9 at 0x60031000 (version 0x09001B00).
  - Registers: 0x0C interrupt mask, 0x14 raw status (bit 0 start, 2 RX, 5 end, 8 skipped), 0x18 ack, 0x1C SLOTCLK (bit 31 samples CLKN in half-slots), 0x24 RX descriptor pointer, 0x100 ACTSCHCNTL (bit 31 starts the event in exchange-table entry [3:0]), 0x2D8 an ESP interrupt FIFO ([9:5] pending, write bit 0 to pop).
  - Exchange memory is the core's own 16-bit offset space, mapped onto DRAM by page registers at 0x60031204+ ([31:18] EM offset / 4, [17:0] DRAM word address; entries above 47 sit 28 bytes further).
- **Exchange-memory layout used:**
  - Exchange-table entry 0 (16 bytes): mode/status [5:3], start time in half-slots, fine start, CS pointer / 2.
  - Scan control structure at 0x0400. The S3 layout is 2 bytes shorter at the start than the public 5.1 headers (no ISOLINKCNTL). +0x16 HOPCNTL: channel [5:0], bit 15 auto-hop (it rotates 37/38/39).
  - RX ring at 0x1000: 10 descriptors × 0x14 bytes. +0 next, bit 15 done; +4 PDU header; +6 RSSI | channel << 8; +8 CLKN at sync; +0x12 payload pointer.
- **BT modem block** at 0x60011000 (0x800 long). Register 0x078 holds the access address.

**What it needed beyond our PHY init**, found by bisecting the vendor bring-up in IDF (`bench/idf_blegen`):

| Step | Result |
|---|---|
| none | 0/10 |
| `esp_bt_controller_init` | 0/10 |
| `+ enable` | 3/10 |
| `esp_phy_enable` alone | works |
| libphy `rf_init` alone | works |

Inside `rf_init`, the piece is `set_chan_freq_hw_init` → `get_rf_freq_init` → `wr_rf_freq_mem`:

1. **RF frequency memory** (write-only; `rf_freq_mem_load()` in `rf_pll.c`). It is the per-MHz LO table the BLE hardware tunes from on every event.
   - 85 entries for 2400–2484 MHz, 3 words each, written through 0x6000E0C4[7:0] (address) / 0x6000E148 (data) / 0x6000E0C4 bit 9 (write).
   - Word 0 is the VCO cap interpolated between our calibrations at 2400 and 2464 MHz, plus `0x80 | vco code`; word 1 is the PLL divider; word 2 is 0.
   - Being write-only, no register dump showed it. Without it the core runs events, but the demodulator never sees the channel.
2. **Coex arbiter page** 0x60035000 (mirrored ×4): grants the radio to BT. Without it the BT demodulator stays idle (readout 0x600110B0 = 0x80).
3. **BT baseband / analog-bus sequencer block** 0x6000E000: loaded while 0x6000E0C4 bit 25 is held, as libbtbb `bt_bb_v2_init_cmplx` does.
4. **Core, modem and exchange-memory image:** generated from a vendor capture (`bench/idf_bleprobe`) by `tools/gen_ble_state.py` into `drivers/ble/ble_state.h`.

**Driving rules learned on the hardware:**
- **Kick lead:** start each event 1 half-slot ahead. A lead of 2 is never picked up, and its kick is silently lost.
- **One window per event:** each event is one receive window, ending after one PDU or about 625 µs, so the core needs one kick per event (queued exchange-table entries don't chain).
- **Drain before each kick:** the core can set a descriptor's done bit after the end interrupt, and an event kicked onto a done descriptor never ends.
- **Handle skips as well as ends:** the interrupt handler masks the core interrupt, handles end and skip (bits 5 and 8), re-arms and unmasks. An unhandled skip leaves the scheduler idle with no further interrupt; that, plus the descriptor race, was why in-handler re-arming first appeared to wedge.
- **Fine start:** each event starts 250 µs after its kick, via the exchange-table fine start (0x60031020 counts down 623..0 in 0.5 µs units). 150 µs is too soon (every event is skipped); this gives 1,190 PDUs/s against 935 with half-slot starts.

**Still open:**
- TX of advertising from the core (software TX works, §10).
- Connections.
- More PDUs per event: chaining receive windows would approach the full 2,500/s.
