/* Sigma-delta (GPIO_SD block). */
#include "sdm.h"
#include "gpio.h"

#define SD_CH(c)   (0x60004F00 + 4 * (c))   /* b7:0 density (int8) b15:8 prescale-1 */
#define SD_CG      0x60004F20               /* b31 clk_en */
#define SD_MISC    0x60004F24               /* b30 function clk_en */
#define SIG_SD0    93

uint32_t sdm_init(int c, int pin, uint32_t hz)
{
    if (c < 0 || c > 7 || !hz || 80000000 % hz || 80000000 / hz > 256) return 0;
    REG_SET(SD_CG, BIT(31));
    REG_SET(SD_MISC, BIT(30));
    REG(SD_CH(c)) = (80000000 / hz - 1) << 8;
    if (gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT) < 0) return 0;
    gpio_out_signal(pin, SIG_SD0 + c, false);
    return hz;
}

RAM_FN void sdm_density(int c, int8_t d) { REG_FIELD(SD_CH(c), 0, 8, (uint8_t)d); }
