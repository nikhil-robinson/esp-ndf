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
