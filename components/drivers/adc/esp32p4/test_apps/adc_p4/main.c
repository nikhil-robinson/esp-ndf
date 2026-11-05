#include "adc.h"
#include "console.h"
#include "sys.h"

static uint32_t buf[4 * 64];

int main(void)
{
    adc_channel(1, 0, ADC_ATTEN_12DB);
    int s = 0;
    for (int i = 0; i < 8; i++) { int r = adc_read_raw(1, 0); printf("one-shot raw %d\n", r); s += r < 0; delay_ms(10); }
    adc_cont_ch_t ch[1] = { { 0, ADC_ATTEN_12DB } };
    if (adc_cont_init(ch, 1, 10000, buf, 64, 4)) { printf("continuous init failed\n"); return 1; }
    adc_cont_start();
    int frames = 0, n;
    for (uint32_t t = time_ms(); time_ms() - t < 200; ) {
        const uint32_t *f = adc_cont_get(&n);
        if (f) { frames++; if (frames == 1) printf("first sample %lu ch %lu\n", (unsigned long)ADC_CONT_DATA(f[0]), (unsigned long)ADC_CONT_CH(f[0])); }
    }
    adc_cont_stop();
    printf("continuous %d frames, overruns %lu, one-shot errors %d\n", frames, (unsigned long)adc_cont_overruns(), s);
    return 0;
}
