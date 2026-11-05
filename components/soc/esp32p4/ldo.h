/* P4 LDO channel switch-on (soc/esp32p4/ldo.c). unit = channel - 1; mv: 500..2700. */
#pragma once
#include "ndf.h"
void ldo_on(int unit, int mv);
