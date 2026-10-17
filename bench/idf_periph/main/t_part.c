#include "t.h"
#include "esp_partition.h"
void t_part(void)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        printf("      %-16s type %02x sub %02x  0x%06lx  %7lu bytes\n", p->label, p->type, p->subtype, p->address, p->size);
    }
}
