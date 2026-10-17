#include "t.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void app_main(void)
{
    vTaskDelay(10);
    printf("\n=== ESP-IDF peripheral timings ===\n");
    printf("[gpio]\n"); t_gpio();
    printf("[gptimer]\n"); t_gptimer();
    printf("[ledc]\n"); t_ledc();
    printf("[mcpwm]\n"); t_mcpwm();
    printf("[pcnt]\n"); t_pcnt();
    printf("[rmt]\n"); t_rmt();
    printf("[sdm]\n"); t_sdm();
    printf("[uart]\n"); t_uart();
    printf("=== done ===\n");
}
