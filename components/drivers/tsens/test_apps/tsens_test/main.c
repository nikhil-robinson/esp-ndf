/* Temperature sensor: eFuse trim, 10 reads (compare with bench/idf_tsens on the same board). */
#include "tsens.h"
#include "efuse.h"
#include "console.h"
#include "sys.h"
#include "cpu.h"

int main(void)
{
    tsens_init();
    printf("tsens_test: BLK_VERSION_MAJOR %lu, TEMP_CALIB 0x%03lx\n", efuse_get("BLK_VERSION_MAJOR"), efuse_get("TEMP_CALIB"));
    float s = 0; uint32_t best = ~0u;
    for (int i = 0; i < 10; i++) {
        uint32_t t = ccount(); float c = tsens_read_c(); t = ccount() - t;
        if (t < best) best = t;
        s += c;
        delay_ms(50);
    }
    int m = (int)(s * 10);
    printf("tsens_test: mean %d.%d C, read %lu cycles\n", m / 100, m % 100 / 10, best);
    printf("tsens_test done\n");
    for (;;) {}
}
