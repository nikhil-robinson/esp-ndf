#include "test.h"
#include "touch.h"
#include "gpio.h"
#include "sys.h"

/* pad 10 = GPIO10 carries the jumper wire + GPIO11's pad: much more capacitance
 * than the bare pads 1 and 14 (same ordering ESP-IDF reports on this board). */
static uint32_t avg(int pad) { uint32_t s = 0; for (int i = 0; i < 16; i++) { s += touch_read(pad); delay_us(2000); } return s / 16; }

void test_touch(void)
{
    gpio_config(11, GPIO_INPUT);                           /* floating */
    CHECK(touch_init(BIT(1) | BIT(10) | BIT(14)) == 0);
    delay_us(50000);
    uint32_t p1 = avg(1), p10 = avg(10), p14 = avg(14);
    printf("    raw: pad1 %lu, pad14 %lu, pad10 %lu (jumper)\n", p1, p14, p10);
    CHECK(p1 > 1000 && p14 > 1000 && p10 > 2 * p1 && p10 > 2 * p14);
    METRIC("touch_read()", "%lu cycles", CYCLES(touch_read(10)));
    gpio_config(11, GPIO_INPUT);
}
