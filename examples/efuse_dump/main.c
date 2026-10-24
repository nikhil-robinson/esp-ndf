/* Every eFuse field of ESP-IDF's table, by name, value (fields > 32 bits as hex bytes). Compared with espefuse summary. */
#include "console.h"
#include "efuse.h"
#include "sys.h"
#include <string.h>
int main(void)
{
    delay_ms(200);
    for (int i = 0; i < efuse_field_count(); i++) {
        const char *n = efuse_field_name(i); int b = efuse_field_bits(i);
        int dup = 0; for (int j = 0; j < i; j++) if (!strcmp(efuse_field_name(j), n)) dup = 1;
        if (dup) continue;
        if (b <= 32) printf("%s = %lu\n", n, (unsigned long)efuse_get(n));
        else { uint8_t v[32]; efuse_read(n, v, sizeof v); printf("%s =", n); for (int k = 0; k < (b + 7) / 8 && k < 32; k++) printf(" %02x", v[k]); printf("\n"); }
    }
    printf("efuse_dump done\n");
    for (;;) {}
}
