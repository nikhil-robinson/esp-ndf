/* On-chip temperature sensor (SAR, ESP32-S3), range -10..80 C (+-1 C, uncalibrated). */
#pragma once
#include "ndf.h"

void  tsens_init(void);
float tsens_read_c(void);                       /* degrees C */
