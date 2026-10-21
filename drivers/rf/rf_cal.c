/* RF calibration store (rf.h "calibration"): the per-chip results of rf_rx_dc_cal (gain-memory DC words), rf_tx_dc_cal
 * (TX DC table) and the crystal correction, kept in NVS (partition "nvs", namespace "rfcal") with the chip's MAC and
 * the die temperature at calibration. rf_open loads them when they belong to this chip and the temperature is within
 * RF_CAL_TEMP_C; otherwise it calibrates and saves. */
#include "rf.h"
#include "rf_init.h"
#include "nvs.h"
#include "efuse.h"
#include "tsens.h"
#include <string.h>

#define RF_CAL_MAGIC 0x52464331u            /* "RFC1" */
typedef struct {
    uint32_t magic;
    uint8_t mac[6];
    int16_t temp_c10;
    int32_t xtal_ppb;
    uint8_t tx_valid, pad[3];
    uint16_t txcal[5][4];
    uint32_t rx_rows[128][2];
} rf_cal_blob_t;

extern uint32_t rf_gain_shadow[256][2];
static rf_cal_blob_t blob;
static int nvs_ok = -1, ts_ok;
static nvs_handle_t h;

static float temp_now(void) { if (!ts_ok) { tsens_init(); ts_ok = 1; } return tsens_read_c(); }
static int store(void)
{
    if (nvs_ok < 0) nvs_ok = !nvs_mount("nvs") && !nvs_open("rfcal", &h);
    return nvs_ok;
}

int rf_cal_save(int tx_valid)
{
    if (!store()) return RF_ERR_STATE;
    memset(&blob, 0, sizeof blob);
    blob.magic = RF_CAL_MAGIC; efuse_mac(blob.mac);
    blob.temp_c10 = (int16_t)(temp_now() * 10);
    blob.xtal_ppb = rf_get_xtal_ppb();
    blob.tx_valid = (uint8_t)tx_valid;
    rf_txcal_get(blob.txcal);
    memcpy(blob.rx_rows, rf_gain_shadow, sizeof blob.rx_rows);
    return nvs_set_blob(h, "v1", &blob, sizeof blob) ? RF_ERR_STATE : RF_OK;
}

/* 1 = applied (rx rows, xtal; *tx_valid says whether the TX table was), 0 = none / other chip / too far in temperature */
int rf_cal_load(int *tx_valid)
{
    if (!store()) return 0;
    size_t n = sizeof blob;
    if (nvs_get_blob(h, "v1", &blob, &n) || n != sizeof blob || blob.magic != RF_CAL_MAGIC) return 0;
    uint8_t mac[6]; efuse_mac(mac);
    if (memcmp(mac, blob.mac, 6)) return 0;
    float dt = temp_now() - blob.temp_c10 / 10.0f;
    if (dt > RF_CAL_TEMP_C || dt < -RF_CAL_TEMP_C) return 0;
    for (int r = 0; r < 128; r++) rf_gain_mem_write((uint8_t)r, blob.rx_rows[r][0], blob.rx_rows[r][1]);
    rf_set_xtal_ppb(blob.xtal_ppb);
    if (blob.tx_valid) rf_txcal_set(blob.txcal);
    if (tx_valid) *tx_valid = blob.tx_valid;
    return 1;
}

int rf_cal_erase(void)
{
    if (!store()) return RF_ERR_STATE;
    memset(&blob, 0, sizeof blob);
    return nvs_set_blob(h, "v1", &blob, sizeof blob) ? RF_ERR_STATE : RF_OK;
}

float rf_cal_temp_c(void) { return blob.magic == RF_CAL_MAGIC ? blob.temp_c10 / 10.0f : -273.0f; }
float rf_temp_c(void) { return temp_now(); }
