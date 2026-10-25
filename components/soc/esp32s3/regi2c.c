/* Analog register bus ("regi2c"): BBPLL, digital LDO, SAR ADC trims.
 * Not in the TRM. Protocol recovered from the S3 ROM (rom_chip_i2c_*):
 *   CONF2 field[16:4] = 0x1FF4, CONF1 = ~block_mask (unmask our block),
 *   CTRL = slave | reg<<8 | data<<16 | START(b26) [| WRITE(b24)],
 *   poll BUSY(b25), data returns in CTRL[23:16]. */
#include "regi2c.h"

/* ROM table at 0x3ff193dc, indexed by block - 0x61 */
static uint32_t block_mask(uint8_t block)
{
    static const uint32_t m[13] = {
        0x00400000, 0x00000800, 0x00000200, 0x00000100, 0x00010000, 0x00020000, 0x00000400,
        0x00008000, 0x00040000, 0x00002000, 0x00001000, 0x00000000, 0x00004000,
    };
    unsigned i = block - 0x61u;
    return i < 13 ? m[i] : 0;
}

static uint32_t select(uint8_t block)
{
    REG(I2C_ANA_CONF2) = (REG(I2C_ANA_CONF2) & 0xFFFE000F) | 0x1FF40;
    REG(I2C_ANA_CONF1) = ~block_mask(block);
    return I2C_ANA_CTRL((block - 0x62u) <= 2u);   /* 0x62..0x64 live on host 1 */
}

static uint32_t xfer(uint32_t ctrl, uint32_t cmd)
{
    for (uint32_t k = 0; (REG(ctrl) & BIT(25)) && k < 100000; k++) {}   /* bounded: I2C master unpowered */
    REG(ctrl) = cmd;
    for (uint32_t k = 0; (REG(ctrl) & BIT(25)) && k < 100000; k++) {}   /* bounded: I2C master unpowered */
    return (REG(ctrl) >> 16) & 0xFF;
}

uint8_t regi2c_read(uint8_t block, uint8_t reg)
{
    return xfer(select(block), block | reg << 8 | BIT(26));
}

void regi2c_write(uint8_t block, uint8_t reg, uint8_t val)
{
    xfer(select(block), block | reg << 8 | (uint32_t)val << 16 | BIT(26) | BIT(24));
}

void regi2c_write_mask(uint8_t block, uint8_t reg, int msb, int lsb, uint8_t val)
{
    uint8_t m = ((1u << (msb - lsb + 1)) - 1) << lsb;
    regi2c_write(block, reg, (regi2c_read(block, reg) & ~m) | ((val << lsb) & m));
}
