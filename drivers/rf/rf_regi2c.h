/* ESP32-S3 analog register bus ("regi2c"): the internal I2C masters that
 * program the RF PLL, BBPLL, LNA/mixer/filters, ADC/DAC and PA.
 * Protocol established from the ROM (rom_chip_i2c_*) and traced on hardware
 * (bench/idf_rftrace): no vendor code involved. */
#pragma once
#include <stdint.h>

/* analog blocks seen in the PHY trace */
#define RF_BLK_BBPLL   0x66   /* plus 0x6A */
#define RF_BLK_RFPLL   0x62   /* VCO cap, calibration control */
#define RF_BLK_RFPLL_N 0x63   /* divider N: reg 3 = integer, 4/5 = 16-bit fraction */
#define RF_BLK_LO      0x65
#define RF_BLK_RXFILT  0x67
#define RF_BLK_TXCAP   0x6B

uint8_t rf_regi2c_read(uint8_t block, uint8_t reg);
void    rf_regi2c_write(uint8_t block, uint8_t reg, uint8_t data);
uint8_t rf_regi2c_read_bits(uint8_t block, uint8_t reg, uint8_t msb, uint8_t lsb);
void    rf_regi2c_write_bits(uint8_t block, uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t data);
