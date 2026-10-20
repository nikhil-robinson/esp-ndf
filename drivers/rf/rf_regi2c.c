/* Analog register bus. Two masters: blocks 0x62/0x63/0x64/0x67/0x6B on
 * master 1, the rest on master 0. Command word at 0x6000E000 + 4 * master:
 *   write: 0x05000000 | data << 16 | reg << 8 | block
 *   read:  0x04000000 | reg << 8 | block, result in bits 23:16
 * bit 25 = busy. Before a read, 0x6000E044 takes ~read_mask(block); selecting
 * master 1 sets 0x6000E048 bits 16:9. */
#include "rf_regi2c.h"
#include "ndf.h"

#undef REG
#define REG(a)        (*(volatile uint32_t *)(a))
#define I2C_MST(n)    (0x6000E000u + 4u * (n))
#define I2C_RD_MASK   0x6000E044u
#define I2C_MST_SEL   0x6000E048u
#define BUSY          (1u << 25)

/* per block 0x61..0x6D, from the ROM table (rom_get_i2c_read_mask) */
static const uint32_t read_mask[13] = { 0x400000, 0x800, 0x200, 0x100, 0x10000, 0x20000, 0x400,
                                        0x8000, 0x40000, 0x2000, 0x1000, 0, 0x4000 };

RAM_FN static unsigned master(uint8_t block)
{
    unsigned i = (uint8_t)(block - 0x62);
    unsigned m = (i <= 9 && ((0x227u >> i) & 1)) ? 1 : 0;
    REG(I2C_MST_SEL) = (REG(I2C_MST_SEL) & 0xFFFE000Fu) | 0x0001FE00u;
    return m;
}

RAM_FN static void wait(unsigned m) { while (REG(I2C_MST(m)) & BUSY) {} }

RAM_FN uint8_t rf_regi2c_read(uint8_t block, uint8_t reg)
{
    unsigned m = master(block), i = (uint8_t)(block - 0x61);
    wait(m);
    REG(I2C_RD_MASK) = ~(i < 13 ? read_mask[i] : 0u);
    REG(I2C_MST(m)) = 0x04000000u | (uint32_t)reg << 8 | block;
    wait(m);
    return (uint8_t)(REG(I2C_MST(m)) >> 16);
}

RAM_FN void rf_regi2c_write(uint8_t block, uint8_t reg, uint8_t data)
{
    unsigned m = master(block);
    wait(m);
    REG(I2C_MST(m)) = 0x05000000u | (uint32_t)data << 16 | (uint32_t)reg << 8 | block;
    wait(m);
}

uint8_t rf_regi2c_read_bits(uint8_t block, uint8_t reg, uint8_t msb, uint8_t lsb)
{
    return (uint8_t)((rf_regi2c_read(block, reg) >> lsb) & ((1u << (msb - lsb + 1)) - 1));
}

void rf_regi2c_write_bits(uint8_t block, uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t data)
{
    uint8_t m = (uint8_t)(((1u << (msb - lsb + 1)) - 1) << lsb);
    uint8_t v = rf_regi2c_read(block, reg);
    rf_regi2c_write(block, reg, (uint8_t)((v & ~m) | ((data << lsb) & m)));
}
