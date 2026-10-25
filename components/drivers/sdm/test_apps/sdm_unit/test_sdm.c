#include "test.h"
#include "sdm.h"
#include "gpio.h"

#define P 4

/* average duty in 0.1 % over 200000 pad samples */
static uint32_t duty(void)
{
    uint32_t hi = 0;
    for (int i = 0; i < 200000; i++) hi += gpio_read(P);
    return hi / 200;
}

void test_sdm(void)
{
    CHECK(sdm_init(0, P, 10000000) == 10000000);
    static const int8_t d[3] = { -64, 0, 64 };
    static const uint32_t want[3] = { 250, 500, 750 };
    for (int k = 0; k < 3; k++) {
        sdm_density(0, d[k]);
        uint32_t m = duty();
        printf("    density %4d: duty %lu.%lu %% (expect %lu %%)\n", d[k], m / 10, m % 10, want[k] / 10);
        CHECK(m + 10 >= want[k] && m <= want[k] + 10);
    }
    METRIC("sdm_density()", "%lu cycles", CYCLES(sdm_density(0, 10)));
    gpio_config(P, GPIO_OUTPUT | GPIO_INPUT);
}

NDF_UNIT(010, sdm);
