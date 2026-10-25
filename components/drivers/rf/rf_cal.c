/* RF calibration store (rf.h "calibration"): the per-chip results of rf_rx_dc_cal (gain-memory DC words), rf_tx_dc_cal
 * (TX DC table) and the crystal correction, kept as one CRC-checked record in the "phy" data partition (as IDF keeps
 * PHY calibration data) with the chip's MAC and the die temperature at calibration. No NVS: its RAM index (16 KB) would
 * crowd out RF apps. rf_open loads them when they belong to this chip and the temperature is within
 * RF_CAL_TEMP_C; otherwise it calibrates and saves. */
#include "rf.h"
#include "rf_init.h"
#include "partition.h"
#include "crc32.h"
#include "efuse.h"
#include "tsens.h"
#include <string.h>
#include <stddef.h>

#define RF_CAL_MAGIC 0x52464331u            /* "RFC1" */
typedef struct {
    uint32_t magic;
    uint8_t mac[6];
    int16_t temp_c10;
    int32_t xtal_ppb;
    uint8_t tx_valid, pad[3];
    uint16_t txcal[5][4];
    uint32_t rx_rows[128][2];
    uint32_t crc;                            /* crc32 of everything before it */
} rf_cal_blob_t;

extern uint32_t rf_gain_shadow[256][2];
static rf_cal_blob_t blob;
static int ts_ok;
static const partition_t *part;

static float temp_now(void) { if (!ts_ok) { tsens_init(); ts_ok = 1; } return tsens_read_c(); }
static int store(void)
{
    if (!part && partition_load() > 0) part = partition_find(PART_DATA, PART_SUB_PHY, "phy");   /* returns the entry count, -1 on a bad table */
    return part != 0;
}
static int put(void)
{
    blob.crc = crc32_le(0, &blob, offsetof(rf_cal_blob_t, crc));
    if (partition_erase(part, 0, 4096)) return RF_ERR_STATE;
    return partition_write(part, 0, &blob, sizeof blob) ? RF_ERR_STATE : RF_OK;
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
    return put();
}

/* 1 = applied (rx rows, xtal; *tx_valid says whether the TX table was), 0 = none / other chip / too far in temperature */
int rf_cal_load(int *tx_valid)
{
    if (!store()) return 0;
    if (partition_read(part, 0, &blob, sizeof blob) || blob.magic != RF_CAL_MAGIC || blob.crc != crc32_le(0, &blob, offsetof(rf_cal_blob_t, crc))) return 0;
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
    return partition_erase(part, 0, 4096) ? RF_ERR_STATE : RF_OK;
}

float rf_cal_temp_c(void) { return blob.magic == RF_CAL_MAGIC ? blob.temp_c10 / 10.0f : -273.0f; }
float rf_temp_c(void) { return temp_now(); }
