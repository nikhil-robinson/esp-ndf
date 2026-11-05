/* SPI2 master timing probe for the logic analyzer: CS = GPIO4 (D1),
 * SCLK = GPIO6 (D2). 4-byte transfers, 10 MHz then 40 MHz, 100 us apart. */
#include "spi.h"
#include "sys.h"
#include "console.h"
#include "cpu.h"

int main(void)
{
    static uint8_t tx[32] = { 0xA5, 0x5A, 0xFF, 0x00 }, rx[32];
    delay_us(1500000);                              /* time to start the capture */
    static const uint32_t f[2] = { 10000000, 40000000 };
    for (int k = 0; k < 2; k++) {
        uint32_t act = spi_master_init(2, 6, 10, 11, 4, f[k], 0);
        uint32_t c = 0;
        for (int i = 0; i < 20; i++) { uint32_t t = ccount(); spi_xfer(2, tx, rx, 4); c = ccount() - t; delay_us(100); }
        uint32_t b2b = ~0u, bad = 0;
        for (int i = 0; i < 200; i++) {                      /* back to back, varying data, check echo (MOSI->MISO jumper 10->11) */
            for (int j = 0; j < 32; j++) tx[j] = i * 7 + j;
            uint32_t t = ccount(); spi_xfer(2, tx, rx, 32); t = ccount() - t;
            if (t < b2b) b2b = t;
            for (int j = 0; j < 32; j++) bad += rx[j] != tx[j];
        }
        printf("spi_probe: %lu Hz: 4-byte xfer %lu cycles; 32-byte back-to-back min %lu cycles (wire %lu), %lu bytes wrong\n",
               act, c, b2b, (uint32_t)(32ull * 8 * sys_cpu_hz() / act), bad);
        delay_us(5000);
    }
    for (;;) ;
}
