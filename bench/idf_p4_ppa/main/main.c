/* IDF sibling of components/drivers/ppa/test_apps/ppa_bench: the same four operations through esp_driver_ppa (blocking), best of 8. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ppa.h"
#include "esp_cpu.h"
#include "esp_cache.h"
static uint16_t big[256 * 128] __attribute__((aligned(64))), small[128 * 64] __attribute__((aligned(64))), rot[64 * 128] __attribute__((aligned(64)));
static uint32_t fg[128 * 64] __attribute__((aligned(64))), bg[128 * 64] __attribute__((aligned(64))), mix[128 * 64] __attribute__((aligned(64)));
static uint32_t us(uint32_t c) { return c / 360; }
void app_main(void)
{
    vTaskDelay(300);
    for (int i = 0; i < 128 * 64; i++) { small[i] = (uint16_t)(i * 31); fg[i] = 0x80000000u | i; bg[i] = 0xFF000000u | (i * 7); }
    esp_cache_msync(small, sizeof small, ESP_CACHE_MSYNC_FLAG_DIR_C2M); esp_cache_msync(fg, sizeof fg, ESP_CACHE_MSYNC_FLAG_DIR_C2M); esp_cache_msync(bg, sizeof bg, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_client_handle_t c; ppa_client_config_t cc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 }, cb = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1 }, cf = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
    ppa_client_handle_t csrm, cblend, cfill;
    ppa_register_client(&cc, &csrm); ppa_register_client(&cb, &cblend); ppa_register_client(&cf, &cfill);
    (void)c;
    ppa_fill_oper_config_t fc = { .out = { .buffer = big, .buffer_size = sizeof big, .pic_w = 256, .pic_h = 128, .fill_cm = PPA_FILL_COLOR_MODE_RGB565 }, .fill_block_w = 256, .fill_block_h = 128,
                                   .fill_argb_color = { .val = 0xF81F }, .mode = PPA_TRANS_MODE_BLOCKING };
    ppa_srm_oper_config_t sc = { .in = { .buffer = small, .pic_w = 128, .pic_h = 64, .block_w = 128, .block_h = 64, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                                 .out = { .buffer = big, .buffer_size = sizeof big, .pic_w = 256, .pic_h = 128, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                                 .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .scale_x = 2.0f, .scale_y = 2.0f, .mode = PPA_TRANS_MODE_BLOCKING };
    ppa_srm_oper_config_t rc = sc; rc.out.buffer = rot; rc.out.buffer_size = sizeof rot; rc.out.pic_w = 64; rc.out.pic_h = 128; rc.rotation_angle = PPA_SRM_ROTATION_ANGLE_90; rc.scale_x = rc.scale_y = 1.0f;
    ppa_blend_oper_config_t bc = { .in_bg = { .buffer = bg, .pic_w = 128, .pic_h = 64, .block_w = 128, .block_h = 64, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
                                   .in_fg = { .buffer = fg, .pic_w = 128, .pic_h = 64, .block_w = 128, .block_h = 64, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
                                   .out = { .buffer = mix, .buffer_size = sizeof mix, .pic_w = 128, .pic_h = 64, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
                                   .mode = PPA_TRANS_MODE_BLOCKING };
    uint32_t tf = ~0u, ts = ~0u, tr = ~0u, tb = ~0u, t; int bad = 0;
    for (int k = 0; k < 8; k++) {
        t = esp_cpu_get_cycle_count(); bad += ppa_do_fill(cfill, &fc) != ESP_OK; t = esp_cpu_get_cycle_count() - t; if (t < tf) tf = t;
        t = esp_cpu_get_cycle_count(); bad += ppa_do_scale_rotate_mirror(csrm, &sc) != ESP_OK; t = esp_cpu_get_cycle_count() - t; if (t < ts) ts = t;
        t = esp_cpu_get_cycle_count(); bad += ppa_do_scale_rotate_mirror(csrm, &rc) != ESP_OK; t = esp_cpu_get_cycle_count() - t; if (t < tr) tr = t;
        t = esp_cpu_get_cycle_count(); bad += ppa_do_blend(cblend, &bc) != ESP_OK; t = esp_cpu_get_cycle_count() - t; if (t < tb) tb = t;
    }
    printf("idf ppa (us, best of 8): fill 256x128 RGB565 %lu | scale 2x 128x64 RGB565 %lu | rotate 90 128x64 RGB565 %lu | blend 128x64 ARGB8888 %lu | errors %d\n",
           (unsigned long)us(tf), (unsigned long)us(ts), (unsigned long)us(tr), (unsigned long)us(tb), bad);
    printf("idf_p4_ppa done\n");
    for (;;) vTaskDelay(1000);
}
