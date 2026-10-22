/* 4.3: transmit an IEEE 802.15.4 O-QPSK frame as a DAC waveform (rf.h: rf_154_build + rf_tx_play).
 * The S3 has no 802.15.4 radio; this plays the synthesised baseband on channel 11 (2405 MHz) and
 * loops it. The waveform is built in PSRAM. Verify by IQ capture on the sibling board (tools/rf_154_test.py).
 *   tx        build the frame and loop the waveform
 *   stop      stop transmitting */
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "psram.h"
#include <string.h>

static const uint8_t psdu[5] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

int main(void)
{
    delay_ms(300);
    uint32_t ps = psram_init(2);
    printf("rf_154 ready open %d psram %lu\n", rf_open(), (unsigned long)ps);
    uint32_t *wave = (uint32_t *)PSRAM_VADDR;
    char line[32]; int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        if (!strcmp(line, "tx")) {
            uint32_t n = rf_154_build(psdu, sizeof psdu, wave, 32768);
            int rc = rf_set_freq_khz(2405000);                /* channel 11 */
            if (!rc) rc = rf_tx_start();
            if (!rc) rc = rf_tx_play(wave, n, 1);             /* loop gaplessly */
            printf("tx samples %lu rc %d\n", (unsigned long)n, rc);
        } else if (!strcmp(line, "stop")) {
            printf("stop %d\n", rf_tx_stop());
        }
        printf("ok\n");
    }
}
