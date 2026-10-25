/* Peripheral self-tests + timing. Each test_x() runs its checks and prints
 * metrics comparable with bench/idf_periph. */
#include "test.h"
#include "sys.h"

int test_fail;

/* tests in run order (later ones depend on state left by earlier ones): unit.h registry, ordered by the
 * section name prefix. PT_FILTER selects tests by name: make APP=examples/periph_test DEFS='-DPT_FILTER="\"pfs\""'
 * (not a console line at boot: esptool's sync bytes, 0x55 = 'U', are still in the USB-JTAG RX FIFO then). */
#ifndef PT_FILTER
#define PT_FILTER NULL
#endif
#define PT(ord, n) static const unit_t __pt_##n __attribute__((used, aligned(4), section(".ndf_reg.test.p" #ord "_" #n))) = \
    { #n, test_##n, __FILE__, __LINE__ };
PT(010, rtcio)
PT(020, gpio)
PT(030, clkout)
PT(040, gptimer)
PT(050, ledc)
PT(060, ledc_gamma)
PT(070, mcpwm)
PT(080, mcpwm_ext)
PT(090, pcnt)
PT(100, pcnt_ext)
PT(110, rmt)
PT(120, rmt_ext)
PT(130, sdm)
PT(140, uart)
PT(150, uart_modes)
PT(160, i2c)
PT(170, i2c_ext)
PT(180, gdma)
PT(190, spi)
PT(200, spi_hd)
PT(210, i2s)
PT(220, i2s_modes)
PT(230, lcd_i80)
PT(240, cam)
PT(250, adc)
PT(260, adc_cont)
PT(270, adc_filter)
PT(280, twai)
PT(290, twai_alerts)
PT(300, touch)
PT(310, touch_ext)
PT(320, touch_denoise)
PT(330, flash)
PT(340, partition)
PT(350, nvs)
PT(360, nvs_enc)
PT(370, fat)
PT(380, fat_ext)
PT(390, spiffs)
PT(400, spiffs_rename)
PT(410, pfs)

int main(void)
{
    printf("\n=== esp-ndf peripheral tests ===\n");
    int fails = unit_run(PT_FILTER);
    printf("=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    return 0;
}
