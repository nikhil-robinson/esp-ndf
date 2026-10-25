/* Wear levelling, IDF v2 on-flash format. Layout at the partition end:
 * [state1][state2][config]. Each state area = 64 B header + one 16 B record
 * per sector; record i written = "dummy sector has passed position i".
 * One spare (dummy) sector rotates through the data area: every 16 erases
 * the sector after it is copied into it and the dummy moves on, so writes
 * spread over the whole partition. */
#include "wl.h"
#include "flash.h"
#include "crc32.h"
#include "xtensa.h"
#include <string.h>

#define SEC         FLASH_SECTOR
#define REC         16              /* pos update record size */
#define UPDATE_RATE 16
#define VERSION     2
#define CRC_LEN     60              /* offsetof(wl_state_t, crc32) */

static uint32_t crc(const void *p, size_t n) { return crc32_le(0xFFFFFFFF, p, n); }

/* record n = crc32 of (device_id + 4n + i), i = 0..3 */
static void rec_fill(wl_t *w, uint32_t n, uint32_t r[4])
{
    for (int i = 0; i < 4; i++) { uint32_t d = w->st.device_id + n * 4 + i; r[i] = crc(&d, 4); }
}

static bool rec_ok(wl_t *w, uint32_t addr, uint32_t n)
{
    uint32_t r[4], e[4];
    if (partition_read(w->part, addr + sizeof(wl_state_t) + n * REC, r, REC)) return false;
    rec_fill(w, n, e);
    return memcmp(r, e, REC) == 0;
}

static int state_write(wl_t *w, uint32_t addr)
{
    if (partition_erase(w->part, addr, w->state_size)) return -1;
    return partition_write(w->part, addr, &w->st, sizeof w->st);
}

/* copy the header and the valid records from one state copy to the other */
static int state_copy(wl_t *w, uint32_t from, uint32_t to)
{
    if (state_write(w, to)) return -1;
    uint32_t r[4];
    for (uint32_t i = 0; i < w->part->size / SEC; i++)
        if (rec_ok(w, from, i)) {
            rec_fill(w, i, r);
            if (partition_write(w->part, to + sizeof(wl_state_t) + i * REC, r, REC)) return -1;
        }
    return 0;
}

static void recover_pos(wl_t *w)
{
    uint32_t i = 0;
    while (i < w->st.max_pos && rec_ok(w, w->addr_state1, i)) i++;
    w->st.dummy_pos = i == w->st.max_pos ? i - 1 : i;
}

static int init_sections(wl_t *w)
{
    memset(&w->st, 0, sizeof w->st);
    w->st.max_erase_count = UPDATE_RATE;
    w->st.version = VERSION;
    w->st.block_size = SEC;
    w->st.device_id = READ_ONCE(REG(0x6003507C)) ^ ccount();   /* RNG_DATA */
    w->st.max_pos = 1 + w->flash_size / SEC;
    w->st.crc32 = crc(&w->st, CRC_LEN);
    if (state_write(w, w->addr_state1) || state_write(w, w->addr_state2)) return -1;

    struct { uint32_t start, size, page, sector, rate, rec, version, tmp, crc, pad[3]; } cfg = {
        0, w->part->size, SEC, SEC, UPDATE_RATE, REC, VERSION, 32, 0, { 0 } };
    cfg.crc = crc(&cfg, 32);
    if (partition_erase(w->part, w->addr_cfg, SEC)) return -1;
    return partition_write(w->part, w->addr_cfg, &cfg, sizeof cfg);
}

/* every UPDATE_RATE erases: copy sector [dummy+1] into the dummy, advance it */
static int update_wl(wl_t *w)
{
    wl_state_t *s = &w->st;
    if (++s->erase_count < s->max_erase_count) return 0;
    s->erase_count = 0;
    uint32_t src = s->dummy_pos + 1 >= s->max_pos ? 0 : s->dummy_pos + 1;
    static uint8_t buf[SEC];
    uint32_t r[4];
    rec_fill(w, s->dummy_pos, r);
    if (partition_erase(w->part, s->dummy_pos * SEC, SEC) ||
        partition_read(w->part, src * SEC, buf, SEC) ||
        partition_write(w->part, s->dummy_pos * SEC, buf, SEC) ||
        partition_write(w->part, w->addr_state1 + sizeof *s + s->dummy_pos * REC, r, REC) ||
        partition_write(w->part, w->addr_state2 + sizeof *s + s->dummy_pos * REC, r, REC)) {
        s->erase_count = s->max_erase_count - 1;          /* retry on the next erase */
        return -1;
    }
    if (++s->dummy_pos >= s->max_pos) {
        s->dummy_pos = 0;
        if (++s->move_count >= s->max_pos - 1) s->move_count = 0;
        s->crc32 = crc(s, CRC_LEN);
        if (state_write(w, w->addr_state1) || state_write(w, w->addr_state2)) return -1;
    }
    return 0;
}

/* logical sector -> physical byte offset in the partition */
static uint32_t phys(wl_t *w, uint32_t sector)
{
    uint32_t a = (w->flash_size - w->st.move_count * SEC + sector * SEC) % w->flash_size;
    return a < w->st.dummy_pos * SEC ? a : a + SEC;
}

static int rd(bdev_t *b, uint32_t s, void *buf, uint32_t n)
{
    wl_t *w = b->ctx;
    if (!bdev_range_ok(b, s, n)) return -1;
    for (uint8_t *p = buf; n--; s++, p += SEC)
        if (partition_read(w->part, phys(w, s), p, SEC)) return -1;
    return 0;
}

static int wr(bdev_t *b, uint32_t s, const void *buf, uint32_t n)
{
    wl_t *w = b->ctx;
    if (!bdev_range_ok(b, s, n)) return -1;
    for (const uint8_t *p = buf; n--; s++, p += SEC) {
        if (update_wl(w)) return -1;
        uint32_t a = phys(w, s);
        if (partition_erase(w->part, a, SEC) || partition_write(w->part, a, p, SEC)) return -1;
    }
    return 0;
}

static int sync_(bdev_t *b) { (void)b; return 0; }   /* state is persisted as it changes */

int wl_mount(wl_t *w, bdev_t *b, const partition_t *p)
{
    if (!p) return -1;
    w->part = p;
    uint32_t need = sizeof(wl_state_t) + p->size / SEC * REC;
    w->state_size = (need + SEC - 1) / SEC * SEC;
    w->addr_cfg = p->size - SEC;
    w->addr_state1 = w->addr_cfg - 2 * w->state_size;
    w->addr_state2 = w->addr_cfg - w->state_size;
    int32_t fs = ((int32_t)(w->addr_state1 / SEC) - 1) * SEC;   /* minus the dummy */
    if (fs <= 0) return -1;
    w->flash_size = fs;

    wl_state_t s2;
    if (partition_read(p, w->addr_state1, &w->st, sizeof w->st) ||
        partition_read(p, w->addr_state2, &s2, sizeof s2)) return -1;
    bool ok1 = crc(&w->st, CRC_LEN) == w->st.crc32, ok2 = crc(&s2, CRC_LEN) == s2.crc32;

    if (ok1 && ok2) {
        if (w->st.version != VERSION) { if (init_sections(w)) return -1; }
        else if (w->st.crc32 != s2.crc32 && state_copy(w, w->addr_state1, w->addr_state2)) return -1;
        recover_pos(w);
    } else if (!ok1 && !ok2) {                       /* blank or foreign: new layout */
        if (init_sections(w)) return -1;           /* ponytail: no v1 -> v2 upgrade */
        recover_pos(w);
    } else if (ok1) {
        if (state_copy(w, w->addr_state1, w->addr_state2)) return -1;
        recover_pos(w);
    } else {
        w->st = s2;
        if (state_copy(w, w->addr_state2, w->addr_state1)) return -1;
        recover_pos(w);
    }
    *b = (bdev_t){ SEC, w->flash_size / SEC, rd, wr, sync_, w };
    return 0;
}
