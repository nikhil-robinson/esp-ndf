/* Analog register bus (regi2c) of the P4, same calls as soc/esp32s3/regi2c.h (block, register, msb, lsb, value). */
#pragma once
#include <stdint.h>

uint8_t regi2c_read(uint8_t block, uint8_t reg);
void    regi2c_write(uint8_t block, uint8_t reg, uint8_t val);
void    regi2c_write_mask(uint8_t block, uint8_t reg, int msb, int lsb, uint8_t val);
