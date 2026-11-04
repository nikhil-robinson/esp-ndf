/* Sigma-delta (GPIO_SD block). */
#include "sdm.h"
#include "gpio.h"

#if SOC_CPU_RISCV    /* ESP32-P4: GPIO_EXT block at 0x500E0F00, same channel layout; no separate clock gate register */
#define SD_BASE    0x500E0F00
#define SIG_SD0    72
#else
#define SD_BASE    0x60004F00
#define SD_CG      (SD_BASE + 0x20)         /* b31 clk_en */
#define SIG_SD0    93
#endif
#define SD_CH(c)   (SD_BASE + 4 * (c))      /* b7:0 density (int8) b15:8 prescale-1 */
#define SD_MISC    (SD_BASE + 0x24)         /* b30 function clk_en */

uint32_t sdm_init(int c, int pin, uint32_t hz)
{
    if (c < 0 || c > 7 || !hz || 80000000 % hz || 80000000 / hz > 256) return 0;
#ifdef SD_CG
    REG_SET(SD_CG, BIT(31));
#endif
    REG_SET(SD_MISC, BIT(30));
    REG(SD_CH(c)) = (80000000 / hz - 1) << 8;
    if (gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT) < 0) return 0;
    gpio_out_signal(pin, SIG_SD0 + c, false);
    return hz;
}

RAM_FN void sdm_density(int c, int8_t d) { REG_FIELD(SD_CH(c), 0, 8, (uint8_t)d); }
