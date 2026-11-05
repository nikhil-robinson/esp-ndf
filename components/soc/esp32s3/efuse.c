/* eFuse read access (TRM ch. 5): the read registers mirror the burnt blocks. Block 0 = RD_WR_DIS + RD_REPEAT_DATA0..4,
 * BLK1 MAC_SPI_SYS (0x44), BLK2 SYS_DATA_PART1 (0x5C), BLK3 user data (0x7C), BLK4..9 keys 0..5 (0x9C + 0x20 k),
 * BLK10 SYS_DATA_PART2 (0x15C). Field names, blocks and bit positions: ESP-IDF's table (efuse_table.h, generated).
 * Burning is deliberately not implemented. */
#include "efuse.h"
#include <string.h>

typedef struct { const char *name; uint8_t block; uint16_t pos, bits; } efuse_field_t;
#include <efuse_table.h>

static uint32_t base(int block)
{
    static const uint16_t off[11] = { 0x2C, 0x44, 0x5C, 0x7C, 0x9C, 0xBC, 0xDC, 0xFC, 0x11C, 0x13C, 0x15C };
#ifndef SOC_EFUSE_BASE
#define SOC_EFUSE_BASE 0x60007000u         /* ESP32-S3; the P4 (same block layout) sets it in soc_caps.h */
#endif
    return SOC_EFUSE_BASE + off[block];
}

uint32_t efuse_bits(int block, int pos, int len)
{
    if (block < 0 || block > 10 || len > 32) return 0;
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        int b = pos + i;
        v |= ((REG(base(block) + 4 * (b / 32)) >> (b % 32)) & 1) << i;
    }
    return v;
}

int efuse_read(const char *name, void *out, size_t out_bytes)    /* bits LSB first into out; returns the field's bits */
{
    uint8_t *o = out;
    int n = 0;                                                    /* fields of several rows (MAC): rows in table order */
    memset(o, 0, out_bytes);
    for (unsigned k = 0; k < sizeof efuse_fields / sizeof efuse_fields[0]; k++) {
        const efuse_field_t *f = &efuse_fields[k];
        if (strcmp(f->name, name)) continue;
        for (int i = 0; i < f->bits; i++, n++)
            if ((size_t)(n / 8) < out_bytes && efuse_bits(f->block, f->pos + i, 1)) o[n / 8] |= 1u << (n % 8);
    }
    return n ? n : -1;
}

uint32_t efuse_get(const char *name)                              /* fields up to 32 bits; 0 if unknown */
{
    uint32_t v = 0;
    return efuse_read(name, &v, sizeof v) > 0 ? v : 0;
}

int efuse_field_count(void) { return (int)(sizeof efuse_fields / sizeof efuse_fields[0]); }
const char *efuse_field_name(int i) { return i >= 0 && i < efuse_field_count() ? efuse_fields[i].name : 0; }
int efuse_field_bits(int i)                                      /* all rows of that name */
{
    if (i < 0 || i >= efuse_field_count()) return 0;
    int n = 0;
    for (int k = 0; k < efuse_field_count(); k++) if (!strcmp(efuse_fields[k].name, efuse_fields[i].name)) n += efuse_fields[k].bits;
    return n;
}

void efuse_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) mac[5 - i] = efuse_bits(1, 8 * i, 8);   /* stored LSB first */
}
