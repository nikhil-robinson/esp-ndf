/* eFuse read access (read-only: burning is deliberately not implemented). Fields by ESP-IDF name (esp_efuse_table.csv:
 * "PSRAM_CAP", "WDT_DELAY_SEL", "FLASH_TYPE", "KEY_PURPOSE_0", ...). */
#pragma once
#include "ndf.h"

uint32_t    efuse_bits(int block, int pos, int len);      /* block 0..10, len <= 32 */
int         efuse_read(const char *name, void *out, size_t out_bytes);   /* field bits (LSB first); -1 unknown name */
uint32_t    efuse_get(const char *name);                  /* a field of up to 32 bits */
int         efuse_field_count(void);
const char *efuse_field_name(int i);
int         efuse_field_bits(int i);
void        efuse_mac(uint8_t mac[6]);
