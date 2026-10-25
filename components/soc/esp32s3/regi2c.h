#pragma once
#include "soc.h"

#define REGI2C_BBPLL   0x66
#define REGI2C_DIG     0x6D

uint8_t regi2c_read(uint8_t block, uint8_t reg);
void    regi2c_write(uint8_t block, uint8_t reg, uint8_t val);
void    regi2c_write_mask(uint8_t block, uint8_t reg, int msb, int lsb, uint8_t val);
