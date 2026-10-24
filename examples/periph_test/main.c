/* Peripheral self-tests + timing. Each test_x() runs its checks and prints
 * metrics comparable with bench/idf_periph. */
#include "test.h"
#include "sys.h"

int test_fail;

static void run(const char *name, void (*fn)(void))
{
    int before = test_fail;
    printf("[%s]\n", name);
    fn();
    printf("  %s %s\n", test_fail == before ? "PASS" : "FAIL", name);
}

int main(void)
{
    printf("\n=== esp-ndf peripheral tests ===\n");
    run("rtcio", test_rtcio);
    run("gpio", test_gpio);
    run("clkout", test_clkout);
    run("gptimer", test_gptimer);
    run("ledc", test_ledc);
    run("ledc_gamma", test_ledc_gamma);
    run("mcpwm", test_mcpwm);
    run("mcpwm_ext", test_mcpwm_ext);
    run("pcnt", test_pcnt);
    run("pcnt_ext", test_pcnt_ext);
    run("rmt", test_rmt);
    run("rmt_ext", test_rmt_ext);
    run("sdm", test_sdm);
    run("uart", test_uart);
    run("uart_modes", test_uart_modes);
    run("i2c", test_i2c);
    run("i2c_ext", test_i2c_ext);
    run("gdma", test_gdma);
    run("spi", test_spi);
    run("spi_hd", test_spi_hd);
    run("i2s", test_i2s);
    run("i2s_modes", test_i2s_modes);
    run("lcd_i80", test_lcd_i80);
    run("cam", test_cam);
    run("adc", test_adc);
    run("adc_cont", test_adc_cont);
    run("adc_filter", test_adc_filter);
    run("twai", test_twai);
    run("twai_alerts", test_twai_alerts);
    run("touch", test_touch);
    run("touch_ext", test_touch_ext);
    run("flash", test_flash);
    run("partition", test_partition);
    run("nvs", test_nvs);
    run("nvs_enc", test_nvs_enc);
    run("fat", test_fat);
    run("fat_ext", test_fat_ext);
    run("spiffs", test_spiffs);
    run("spiffs_rename", test_spiffs_rename);
    run("pfs", test_pfs);
    printf("=== %s (%d failures) ===\n", test_fail ? "FAILED" : "ALL PASS", test_fail);
    return 0;
}
