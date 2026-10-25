/* AFC: lock our LO to a reference transmitter (D0: tone at LO 2437 MHz + 2 MHz) with the public RF API.
 * Every 0.5 s: measure the tone near +3 MHz (we sit at 2436 MHz), steer the crystal correction. */
#include "console.h"
#include "sys.h"
#include "rf.h"

int main(void)
{
    delay_ms(500);
    printf("=== rf_afc\n");
    rf_open();
    rf_set_freq_khz(2436000);
    rf_rx_start();
    rf_rx_set_gain(10);
    uint64_t t0 = time_us();
    for (int k = 0; k < 400; k++) {
        float hz; if (rf_rx_tone_hz(3e6f, &hz)) continue;
        float off = hz - 3e6f;
        int32_t ppb = k < 2 ? rf_get_xtal_ppb() : rf_afc_step(off, 0.7f);
        printf("afc %llu ms offset %.1f Hz ppb %ld\n", (unsigned long long)((time_us() - t0) / 1000), off, (long)ppb);
        delay_ms(500);
    }
    printf("=== done\n");
    for (;;) {}
}
