/* Partition table: up to 95 entries of 32 bytes at 0x8000, then optional
 * MD5 entry (magic 0xEBEB, 14 x 0xFF, digest of all entries before it). */
#include "partition.h"
#include "flash.h"
#include "md5.h"

#define TABLE_ADDR NDF_PART_TABLE_ADDR                     /* from app.mk (default 0x80000) */
#define TABLE_MAX  95

static partition_t parts[TABLE_MAX];
static int count = -1;
static uint8_t raw[TABLE_MAX * 32 + 32] __attribute__((aligned(4)));

int partition_load(void)
{
    if (flash_read(TABLE_ADDR, raw, sizeof raw)) return -1;
    int n = 0;
    for (; n < TABLE_MAX; n++) {
        const uint8_t *e = raw + 32 * n;
        uint16_t magic = e[0] | e[1] << 8;
        if (magic == 0xEBEB) {                               /* checksum entry */
            uint8_t d[16];
            md5(raw, 32 * n, d);
            for (int i = 0; i < 16; i++) if (d[i] != e[16 + i]) { count = -1; return -1; }
            break;
        }
        if (magic != 0x50AA) break;
        partition_t *p = &parts[n];
        p->type = e[2]; p->subtype = e[3];
        p->offset = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
        p->size   = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        for (int i = 0; i < 16; i++) p->label[i] = e[12 + i];
        p->label[16] = 0;
        p->flags  = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t)e[31] << 24;
    }
    return count = n;
}

const partition_t *partition_get(int i) { return i >= 0 && i < count ? &parts[i] : 0; }

static bool label_eq(const char *a, const char *b)
{
    for (int i = 0; i < 16; i++) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    return true;
}

const partition_t *partition_find(uint8_t type, uint8_t sub, const char *label)
{
    if (count < 0 && partition_load() < 0) return 0;
    for (int i = 0; i < count; i++)
        if (parts[i].type == type && (sub == PART_SUB_ANY || parts[i].subtype == sub) &&
            (!label || label_eq(parts[i].label, label)))
            return &parts[i];
    return 0;
}

static bool in(const partition_t *p, uint32_t off, size_t len) { return p && off <= p->size && len <= p->size - off; }

int partition_read(const partition_t *p, uint32_t off, void *b, size_t n)
{
    return in(p, off, n) ? flash_read(p->offset + off, b, n) : -1;
}

int partition_write(const partition_t *p, uint32_t off, const void *b, size_t n)
{
    return in(p, off, n) ? flash_write(p->offset + off, b, n) : -1;
}

int partition_erase(const partition_t *p, uint32_t off, size_t n)
{
    if (!in(p, off, n) || (off | n) & (FLASH_SECTOR - 1)) return -1;
    for (size_t k = 0; k < n; k += FLASH_SECTOR)
        if (flash_erase_sector(p->offset + off + k)) return -1;
    return 0;
}
