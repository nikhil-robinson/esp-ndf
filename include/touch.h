/* Capacitive touch: pads 1..14 = GPIO1..14, hardware scan FSM, raw counts
 * (higher = more capacitance on S3). */
#pragma once
#include "ndf.h"

int      touch_init(uint16_t pad_mask);       /* bit n = pad n; starts scanning */
uint32_t touch_read(int pad);                 /* latest raw count */
