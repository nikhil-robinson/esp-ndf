/* esp-ndf glue for the RF driver (the IDF benches define their own). */
#include "sys.h"
#include "rf_init.h"

void rf_delay_us(uint32_t us) { delay_us(us); }
