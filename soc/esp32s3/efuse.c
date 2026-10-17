/* eFuse: the read registers mirror the burnt blocks (TRM ch. 5).
 * BLK1 (MAC_SPI_SYS) at 0x60007044, BLK2 (SYS_DATA_PART1) at 0x6000705C. */
#include "efuse.h"

static uint32_t base(int block) { return block == 1 ? 0x60007044 : 0x6000705C; }

uint32_t efuse_bits(int block, int pos, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        int b = pos + i;
        v |= ((REG(base(block) + 4 * (b / 32)) >> (b % 32)) & 1) << i;
    }
    return v;
}

void efuse_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) mac[5 - i] = efuse_bits(1, 8 * i, 8);   /* stored LSB first */
}
