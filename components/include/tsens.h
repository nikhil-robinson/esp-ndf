/* On-chip temperature sensor (SAR, ESP32-S3), range -10..100 C, eFuse per-chip trim applied. */
#pragma once
#include "ndf.h"

void  tsens_init(void);
float tsens_read_c(void);                       /* degrees C */
