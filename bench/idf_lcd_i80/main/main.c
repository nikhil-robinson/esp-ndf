/* IDF sibling of examples/periph_test/test_lcd_i80.c: i80 8-bit on 16 17 18 21 38 39 40 41, WR 14, DC 15. */
#include <stdio.h>
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_i80_bus_config_t bc = { .dc_gpio_num = 15, .wr_gpio_num = 14, .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = { 16, 17, 18, 21, 38, 39, 40, 41 }, .bus_width = 8, .max_transfer_bytes = 4096 };
    esp_lcd_new_i80_bus(&bc, &bus);
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i80_config_t ic = { .cs_gpio_num = -1, .pclk_hz = 10000000, .trans_queue_depth = 4,
        .dc_levels = { .dc_data_level = 1 }, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    esp_lcd_new_panel_io_i80(bus, &ic, &io);
    uint8_t *px = heap_caps_malloc(300, MALLOC_CAP_DMA);
    uint32_t best = ~0u;
    for (int k = 0; k < 8; k++) {
        uint32_t c = esp_cpu_get_cycle_count();
        esp_lcd_panel_io_tx_color(io, 0x2C, px, 300);
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
        vTaskDelay(2);
    }
    printf("tx_color 300 B start: %lu cycles\n", best);
    printf("idf_lcd_i80 done\n");
}
