#include "test.h"
#include "partition.h"
#include "md5.h"

void test_partition(void)
{
    uint8_t d[16];
    md5("abc", 3, d);                                   /* RFC 1321 test vector */
    CHECK(d[0] == 0x90 && d[1] == 0x01 && d[15] == 0x72);
    int n = partition_load();
    printf("    %d partitions (MD5 verified):\n", n);
    CHECK(n > 0);
    for (int i = 0; i < n; i++) {
        const partition_t *p = partition_get(i);
        printf("      %-16s type %02x sub %02x  0x%06lx  %7lu bytes\n", p->label, p->type, p->subtype, p->offset, p->size);
    }
    CHECK(partition_find(PART_DATA, PART_SUB_NVS, 0) != 0);
    const partition_t *nvs = partition_find(PART_DATA, PART_SUB_NVS, "nvs");
    uint8_t b[4];
    CHECK(nvs && partition_read(nvs, nvs->size - 2, b, 4) == -1);   /* out of bounds refused */
}

NDF_UNIT(010, partition);
