#include "t.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void app_main(void)
{
    vTaskDelay(10);
    printf("\n=== ESP-IDF peripheral timings ===\n");
    printf("[touch]\n"); t_touch();
    printf("[twai]\n"); t_twai();
    printf("[adc]\n"); t_adc();
    printf("[i2s]\n"); t_i2s();
    printf("[spi]\n"); t_spi();
    printf("[gpio]\n"); t_gpio();
    printf("[gptimer]\n"); t_gptimer();
    printf("[ledc]\n"); t_ledc();
    printf("[mcpwm]\n"); t_mcpwm();
    printf("[pcnt]\n"); t_pcnt();
    printf("[rmt]\n"); t_rmt();
    printf("[sdm]\n"); t_sdm();
    printf("[uart]\n"); t_uart();
    printf("[i2c]\n"); t_i2c();
    printf("[gdma]\n"); t_gdma();
    printf("=== done ===\n");
}
