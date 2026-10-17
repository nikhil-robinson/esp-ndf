/* Same benchmark as bm/, on ESP-IDF + FreeRTOS: one max-priority task pinned per core. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#define BENCH_FN IRAM_ATTR
#include "../../bench.h"

static int bench_usb_tx(const char *buf, int n)
{
    return usb_serial_jtag_write_bytes(buf, n, 0);   /* non-blocking */
}

static bool IRAM_ATTR timer_cb(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    bench_isr();
    return false;
}

static void timer_start(void)   /* runs on core0, so the ISR lands on core0 */
{
    gptimer_handle_t t;
    gptimer_config_t cfg = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 10000000 };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &t));
    gptimer_alarm_config_t al = { .alarm_count = 10000000 / ISR_HZ, .flags.auto_reload_on_alarm = true };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(t, &al));
    gptimer_event_callbacks_t cb = { .on_alarm = timer_cb };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(t, &cb, NULL));
    ESP_ERROR_CHECK(gptimer_enable(t));
    ESP_ERROR_CHECK(gptimer_start(t));
}

static void bench_task(void *arg)
{
#ifdef BENCH_IRQ_OFF
    /* hybrid: core0 keeps the RTOS, core1 masks IRQs and runs bare.
     * (masking on core0 too hangs the system: the RTOS needs core0) */
    if ((int)arg == 1) portDISABLE_INTERRUPTS();
#endif
    bench_run((int)arg);
#ifdef BENCH_IRQ_OFF
    if ((int)arg == 1) portENABLE_INTERRUPTS();
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    gpio_set_direction(BENCH_PIN(0), GPIO_MODE_OUTPUT);
    gpio_set_direction(BENCH_PIN(1), GPIO_MODE_OUTPUT);
    gpio_set_direction(ISR_PIN, GPIO_MODE_OUTPUT);
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));
    timer_start();
    /* core1 first: the core0 task preempts app_main the moment it exists */
    xTaskCreatePinnedToCore(bench_task, "b1", 4096, (void *)1, configMAX_PRIORITIES - 1, NULL, 1);
    xTaskCreatePinnedToCore(bench_task, "b0", 4096, (void *)0, configMAX_PRIORITIES - 1, NULL, 0);
    while (!*(volatile uint32_t *)&bench_res[0].work_per_s || !*(volatile uint32_t *)&bench_res[1].work_per_s) vTaskDelay(10);
    #ifdef BENCH_IRQ_OFF
    printf("\n[hybrid: interrupts masked on core1]");
#endif
    printf("\nFreeRTOS tick %d Hz", CONFIG_FREERTOS_HZ);
    bench_print(printf, "FREERTOS");
}
