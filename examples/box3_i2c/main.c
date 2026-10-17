/* BOX-3 internal I2C bus scan (SCL 18, SDA 8, board pull-ups) at 100 k and 400 k. */
#include "i2c.h"
#include "console.h"

int main(void)
{
    static const uint32_t f[2] = { 100000, 400000 };
    for (int k = 0; k < 2; k++) {
        uint32_t act = i2c_master_init(0, 8, 18, f[k], false);
        printf("\nbox3_i2c: scan @%lu Hz:", act);
        int n = 0;
        for (int a = 0x08; a < 0x78; a++) {
            uint8_t dummy;
            /* probe: address + 1-byte read; ACK on the address means present */
            if (i2c_master_xfer(0, a, 0, 0, &dummy, 1, 20000) == I2C_OK) { printf(" 0x%02x", a); n++; }
        }
        printf("  (%d devices)\n", n);
        /* register reads: write reg address, repeated start, read */
        uint8_t reg = 0x75, who = 0, id1 = 0, id2 = 0;
        i2c_err_t e = i2c_master_xfer(0, 0x68, &reg, 1, &who, 1, 20000);
        reg = 0xFD; i2c_master_xfer(0, 0x18, &reg, 1, &id1, 1, 20000);   /* ES8311 CHIP ID1 */
        reg = 0xFE; i2c_master_xfer(0, 0x18, &reg, 1, &id2, 1, 20000);   /* ES8311 CHIP ID2 */
        printf("box3_i2c: IMU WHO_AM_I = 0x%02x (err %d), ES8311 chip id = %02x %02x\n", who, e, id1, id2);
    }
    return 0;
}
