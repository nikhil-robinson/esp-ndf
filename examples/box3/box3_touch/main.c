/* BOX-3 GT911 touch controller: id, resolution, report reads (touch the screen to see points). */
#include "i2c.h"
#include "gt911.h"
#include "console.h"
#include "sys.h"
#include "xtensa.h"

int main(void)
{
    i2c_master_init(0, 8, 18, 400000, false);
    char id[5]; uint16_t xr = 0, yr = 0;
    int e = gt911_init(0, 0x5D, id, &xr, &yr);
    printf("box3_touch: GT911 init %d, product id \"%s\", resolution %u x %u\n", e, id, xr, yr);
    gt911_point_t p[5];
    uint32_t best = ~0u; int seen = 0;
    for (int k = 0; k < 200; k++) {                         /* 2 s */
        uint32_t t = ccount();
        int n = gt911_read(p, 5);
        t = ccount() - t;
        if (t < best) best = t;
        if (n > 0) { seen++; printf("  %d point(s): (%u, %u)\n", n, p[0].x, p[0].y); }
        delay_ms(10);
    }
    printf("box3_touch: report poll %lu cycles (400 kHz), %d reports with touches in 2 s\n", best, seen);
    printf("box3_touch %s done\n", e == 0 && id[0] == '9' && xr && yr ? "PASS" : "FAIL");
    for (;;) {}
}
