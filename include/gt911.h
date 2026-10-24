/* GT911 capacitive touch controller (BOX-3 at 0x5D on the internal I2C), up to 5 points. */
#pragma once
#include "ndf.h"

typedef struct { uint16_t x, y, size; uint8_t id; } gt911_point_t;

/* i2c port must already be up (i2c_master_init). Returns 0 and fills id ("911") / resolution. */
int gt911_init(int port, uint8_t addr, char id[5], uint16_t *xres, uint16_t *yres);
/* Points touched now (0..5), -1 on bus error. Acknowledges the report. */
int gt911_read(gt911_point_t *pts, int max);
