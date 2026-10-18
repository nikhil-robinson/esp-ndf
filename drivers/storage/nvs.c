/* NVS, ESP-IDF on-flash format v2 (page version 0xFE).
 * Page: [0] state [4] seq [8] version [9..27] 0xFF [28] crc32(4..27)
 *       [32..63] entry state bitmap, 2 bits/entry: 3 empty, 2 written, 0 erased
 *       [64 + 32 i] 126 entries.
 * Item: ns, type, span, chunk, crc32, key[16], data[8]; strings/blob data
 * continue in the following span-1 entries.
 * A RAM index of written items makes lookups flash-free.
 * Encryption (optional, IDF-compatible): every 32 B entry is XTS-AES-256
 * encrypted with its partition-relative address as tweak; page headers and
 * bitmaps stay plain. */
#include "nvs.h"
#include "partition.h"
#include "flash.h"
#include "crc32.h"
#include "aes.h"
#include <string.h>

#define PAGE        4096
#define ENTRIES     126
#define MAXPAGES    16                  /* 64 KB partitions max */
#define ST_EMPTY    0xFFFFFFFFu
#define ST_ACTIVE   0xFFFFFFFEu
#define ST_FULL     0xFFFFFFFCu
#define ST_FREEING  0xFFFFFFF8u
#define VERSION     0xFE

enum { T_U8 = 0x01, T_I8 = 0x11, T_U16 = 0x02, T_I16 = 0x12, T_U32 = 0x04, T_I32 = 0x14,
       T_U64 = 0x08, T_I64 = 0x18, T_SZ = 0x21, T_BLOB_DATA = 0x42, T_BLOB_IDX = 0x48 };
#define CHUNK_ANY   0xFF

typedef struct __attribute__((packed)) {
    uint8_t ns, type, span, chunk;
    uint32_t crc;
    char key[16];
    union {
        uint8_t data[8];
        struct __attribute__((packed)) { uint16_t size, rsv; uint32_t crc; } var;
        struct __attribute__((packed)) { uint32_t size; uint8_t count, start; uint16_t rsv; } blob;
    };
} item_t;

typedef struct { uint32_t state, seq; uint32_t bitmap[8]; uint8_t next; } page_t;
typedef struct { uint8_t page, idx, ns, type, span, chunk; uint16_t hash; } ent_t;

static const partition_t *part;
static int npages, active = -1;
static page_t pg[MAXPAGES];
static ent_t idx[MAXPAGES * ENTRIES];
static int nidx;
static uint32_t max_seq;
static const uint8_t *xkey;             /* 64 B XTS key, NULL = plain */

static uint32_t addr_of(int p, int e) { return p * PAGE + 64 + 32 * e; }
static int  est(int p, int e)         { return pg[p].bitmap[e / 16] >> (2 * (e % 16)) & 3; }
static uint16_t khash(const char *k)  { return crc32_le(0, k, strnlen(k, 16)) & 0xFFFF; }

static uint32_t item_crc(const item_t *it)
{
    const uint8_t *p = (const uint8_t *)it;
    uint32_t c = crc32_le(0xFFFFFFFF, p, 4);
    c = crc32_le(c, p + 8, 16);
    return crc32_le(c, p + 24, 8);
}

/* entry area I/O: addr is entry-aligned; reads may stop mid-entry */
static int ent_rd(uint32_t addr, void *buf, size_t len)
{
    if (!xkey) return partition_read(part, addr, buf, len);
    uint8_t t[32];
    for (size_t off = 0; off < len; off += 32) {
        if (partition_read(part, addr + off, t, 32)) return -1;
        aes_xts(xkey, addr + off, false, t, t, 32);
        memcpy((uint8_t *)buf + off, t, len - off < 32 ? len - off : 32);
    }
    return 0;
}

static int ent_wr(uint32_t addr, const void *buf, size_t len)      /* len: whole entries */
{
    if (!xkey) return partition_write(part, addr, buf, len);
    uint8_t t[32];
    for (size_t off = 0; off < len; off += 32) {
        aes_xts(xkey, addr + off, true, (const uint8_t *)buf + off, t, 32);
        if (partition_write(part, addr + off, t, 32)) return -1;
    }
    return 0;
}

/* ---- page level ---------------------------------------------------------- */
static int set_state(int p, int e, int from, int to, int st)  /* entries [from, to) -> st */
{
    for (int i = from; i < to; i++) {
        uint32_t *w = &pg[p].bitmap[i / 16];
        *w &= ~(3u << (2 * (i % 16))) | ((uint32_t)st << (2 * (i % 16)));
    }
    (void)e;
    int w0 = from / 16, w1 = (to - 1) / 16;                   /* program only the words that changed */
    return partition_write(part, p * PAGE + 32 + 4 * w0, &pg[p].bitmap[w0], 4 * (w1 - w0 + 1));
}

static int page_state(int p, uint32_t st)
{
    pg[p].state = st;
    return partition_write(part, p * PAGE, &st, 4);
}

static int activate(int p)                                    /* blank page -> ACTIVE */
{
    uint8_t h[32];
    memset(h, 0xFF, 32);
    uint32_t seq = ++max_seq;
    memcpy(h + 4, &seq, 4);
    h[8] = VERSION;
    uint32_t c = crc32_le(0xFFFFFFFF, h + 4, 24);
    memcpy(h + 28, &c, 4);
    if (partition_write(part, p * PAGE + 4, h + 4, 28)) return -1;
    if (page_state(p, ST_ACTIVE)) return -1;
    pg[p].seq = seq;
    for (int i = 0; i < 8; i++) pg[p].bitmap[i] = ~0u;
    pg[p].next = 0;
    active = p;
    return 0;
}

static bool blank(int p)
{
    uint32_t b[16];
    for (int off = 0; off < PAGE; off += 64) {
        if (partition_read(part, p * PAGE + off, b, 64)) return false;
        for (int i = 0; i < 16; i++) if (b[i] != ~0u) return false;
    }
    return true;
}

static int empty_pages(void)
{
    int n = 0;
    for (int p = 0; p < npages; p++) n += pg[p].state == ST_EMPTY;
    return n;
}

static int index_page(int p)                                  /* add page p's written items */
{
    for (int e = 0; e < ENTRIES; ) {
        if (est(p, e) != 2) { e++; continue; }
        item_t it;
        if (ent_rd(addr_of(p, e), &it, 32)) return -1;
        int span = it.span ? it.span : 1;
        if (it.crc == item_crc(&it) && span <= ENTRIES - e) {
            idx[nidx++] = (ent_t){ p, e, it.ns, it.type, span, it.chunk, khash(it.key) };
        }
        e += span;
    }
    return 0;
}

/* Garbage collection: move live items of the FULL page with most erased
 * entries into a fresh page, then erase it (it becomes the spare). */
static int gc(void)
{
    int victim = -1, best = 0;
    for (int p = 0; p < npages; p++) {
        if (pg[p].state != ST_FULL) continue;
        int er = 0;
        for (int e = 0; e < ENTRIES; e++) er += est(p, e) == 0;
        if (er > best) { best = er; victim = p; }
    }
    int dst = -1;
    for (int p = 0; p < npages; p++) if (pg[p].state == ST_EMPTY) { dst = p; break; }
    if (victim < 0 || dst < 0) return NVS_NO_SPACE;
    if (page_state(victim, ST_FREEING) || activate(dst)) return NVS_ERR;
    uint8_t buf[32];
    for (int i = 0; i < nidx; i++) {
        if (idx[i].page != victim) continue;
        int e = pg[dst].next;
        for (int s = 0; s < idx[i].span; s++) {
            if (ent_rd(addr_of(victim, idx[i].idx + s), buf, 32) ||
                ent_wr(addr_of(dst, e + s), buf, 32)) return NVS_ERR;
        }
        if (set_state(dst, 0, e, e + idx[i].span, 2)) return NVS_ERR;
        pg[dst].next += idx[i].span;
        idx[i].page = dst; idx[i].idx = e;
    }
    if (partition_erase(part, victim * PAGE, PAGE)) return NVS_ERR;
    pg[victim].state = ST_EMPTY;
    for (int k = 0; k < 8; k++) pg[victim].bitmap[k] = ~0u;
    return 0;
}

/* make sure the active page has `span` contiguous free entries */
static int room(int span)
{
    if (span > ENTRIES) return NVS_TOO_BIG;
    if (active >= 0 && pg[active].next + span <= ENTRIES) return 0;
    if (active >= 0 && page_state(active, ST_FULL)) return NVS_ERR;
    active = -1;
    if (empty_pages() < 2) { int r = gc(); if (r) return r; if (pg[active].next + span <= ENTRIES) return 0; page_state(active, ST_FULL); active = -1; }
    if (empty_pages() < 2) return NVS_NO_SPACE;               /* always keep one spare for GC */
    for (int p = 0; p < npages; p++)
        if (pg[p].state == ST_EMPTY) {
            if (!blank(p) && partition_erase(part, p * PAGE, PAGE)) return NVS_ERR;
            return activate(p);
        }
    return NVS_NO_SPACE;
}

static int nvs_mount_(const char *label);

/* ---- items --------------------------------------------------------------- */
static int find(uint8_t ns, uint8_t type, const char *k, uint8_t chunk, item_t *out)
{
    uint16_t h = khash(k);
    int hit = -1;
    for (int i = 0; i < nidx; i++) {
        ent_t *x = &idx[i];
        if (x->hash != h || x->ns != ns || (type != 0xFF && x->type != type) || (chunk != CHUNK_ANY && x->chunk != chunk)) continue;
        item_t it;
        if (ent_rd(addr_of(x->page, x->idx), &it, 32) || strncmp(it.key, k, 16)) continue;
        /* duplicates (crash between write-new and erase-old): newest wins */
        if (hit < 0 || pg[x->page].seq > pg[idx[hit].page].seq ||
            (x->page == idx[hit].page && x->idx > idx[hit].idx)) { hit = i; if (out) *out = it; }
    }
    return hit;
}

static int erase_ent(int i)
{
    ent_t *x = &idx[i];
    if (set_state(x->page, 0, x->idx, x->idx + x->span, 0)) return NVS_ERR;
    idx[i] = idx[--nidx];
    return 0;
}

static int write_item(uint8_t ns, uint8_t type, const char *k, uint8_t chunk, const uint8_t data8[8],
                      const void *payload, size_t len)
{
    int span = 1 + (int)((len + 31) / 32);
    int r = room(span);
    if (r) return r;
    item_t it;
    memset(&it, 0xFF, sizeof it);
    it.ns = ns; it.type = type; it.span = span; it.chunk = chunk;
    memset(it.key, 0, 16);
    strncpy(it.key, k, 15);
    if (payload) {
        it.var.size = len; it.var.rsv = 0xFFFF;
        it.var.crc = crc32_le(0xFFFFFFFF, payload, len);
    } else {
        memcpy(it.data, data8, 8);
    }
    it.crc = item_crc(&it);
    int e = pg[active].next;
    if (payload) {
        static uint8_t tmp[32] __attribute__((aligned(4)));
        for (size_t off = 0; off < len; off += 32) {          /* data entries, padded with 0xFF */
            size_t n = len - off < 32 ? len - off : 32;
            memset(tmp, 0xFF, 32);
            memcpy(tmp, (const uint8_t *)payload + off, n);
            if (ent_wr(addr_of(active, e + 1 + off / 32), tmp, 32)) return NVS_ERR;
        }
    }
    if (ent_wr(addr_of(active, e), &it, 32)) return NVS_ERR;
    if (set_state(active, 0, e, e + span, 2)) return NVS_ERR;
    pg[active].next += span;
    idx[nidx++] = (ent_t){ active, e, ns, type, span, chunk, khash(k) };
    return 0;
}

/* erase the index entry sitting at (page, entry), if any */
static int erase_at(int page, int e)
{
    for (int i = 0; i < nidx; i++) if (idx[i].page == page && idx[i].idx == e) return erase_ent(i);
    return 0;
}

static int set_prim(nvs_handle_t ns, const char *k, uint8_t type, const void *v, int n)
{
    if (!part || !ns || !k || !*k) return NVS_ERR;
    uint8_t d[8];
    memset(d, 0xFF, 8);
    memcpy(d, v, n);
    item_t old;
    int o = find(ns, type, k, CHUNK_ANY, &old);
    if (o >= 0 && !memcmp(old.data, d, 8)) return 0;          /* unchanged: no flash wear */
    int op = o >= 0 ? idx[o].page : -1, oe = o >= 0 ? idx[o].idx : -1;
    int r = write_item(ns, type, k, CHUNK_ANY, d, 0, 0);       /* new first, then erase old */
    if (r) return r;
    return o >= 0 ? erase_at(op, oe) : 0;
}

static int get_prim(nvs_handle_t ns, const char *k, uint8_t type, void *v, int n)
{
    item_t it;
    if (!part || find(ns, type, k, CHUNK_ANY, &it) < 0) return NVS_NOT_FOUND;
    memcpy(v, it.data, n);
    return 0;
}

#define PRIM(name, T, code) \
    int nvs_set_##name(nvs_handle_t h, const char *k, T v) { return set_prim(h, k, code, &v, sizeof v); } \
    int nvs_get_##name(nvs_handle_t h, const char *k, T *v) { return get_prim(h, k, code, v, sizeof *v); }
PRIM(u8, uint8_t, T_U8)   PRIM(i8, int8_t, T_I8)   PRIM(u16, uint16_t, T_U16) PRIM(i16, int16_t, T_I16)
PRIM(u32, uint32_t, T_U32) PRIM(i32, int32_t, T_I32) PRIM(u64, uint64_t, T_U64) PRIM(i64, int64_t, T_I64)

static int erase_all_of(uint8_t ns, const char *k, uint8_t type)
{
    uint16_t h = khash(k);
    for (int i = nidx - 1; i >= 0; i--) {
        if (idx[i].ns != ns || idx[i].hash != h || (type != 0xFF && idx[i].type != type)) continue;
        item_t t;
        ent_rd(addr_of(idx[i].page, idx[i].idx), &t, 32);
        if (!strncmp(t.key, k, 16) && erase_ent(i)) return NVS_ERR;
    }
    return 0;
}

int nvs_set_str(nvs_handle_t ns, const char *k, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > 4000) return NVS_TOO_BIG;
    item_t old;
    int o = find(ns, T_SZ, k, CHUNK_ANY, &old);
    if (o >= 0 && old.var.size == n && old.var.crc == crc32_le(0xFFFFFFFF, s, n)) return 0;
    /* write new, then erase the old instance (identified by position) */
    int op = o >= 0 ? idx[o].page : -1, oe = o >= 0 ? idx[o].idx : -1;
    int r = write_item(ns, T_SZ, k, CHUNK_ANY, 0, s, n);
    if (r) return r;
    return o >= 0 ? erase_at(op, oe) : 0;
}

int nvs_get_str(nvs_handle_t ns, const char *k, char *s, size_t *len)
{
    item_t it;
    int i = find(ns, T_SZ, k, CHUNK_ANY, &it);
    if (i < 0) return NVS_NOT_FOUND;
    if (!s) { *len = it.var.size; return 0; }
    if (*len < it.var.size) { *len = it.var.size; return NVS_TOO_BIG; }
    if (ent_rd(addr_of(idx[i].page, idx[i].idx + 1), s, it.var.size)) return NVS_ERR;
    if (crc32_le(0xFFFFFFFF, s, it.var.size) != it.var.crc) return NVS_ERR;
    *len = it.var.size;
    return 0;
}

/* blob v2: BLOB_DATA chunks (chunk index = start + n) + one BLOB_IDX.
 * start alternates 0x00 / 0x80 between versions so old and new coexist. */
int nvs_set_blob(nvs_handle_t ns, const char *k, const void *b, size_t n)
{
    item_t old;
    int o = find(ns, T_BLOB_IDX, k, CHUNK_ANY, &old);
    uint8_t start = (o >= 0 && old.blob.start == 0) ? 0x80 : 0;
    uint8_t count = 0;
    size_t off = 0;
    do {
        if (room(2)) return NVS_NO_SPACE;
        size_t fit = (size_t)(ENTRIES - pg[active].next - 1) * 32;
        size_t c = n - off < fit ? n - off : fit;
        if (c > 4000) c = 4000;
        int r = write_item(ns, T_BLOB_DATA, k, start + count, 0, (const uint8_t *)b + off, c);
        if (r) return r;
        off += c; count++;
    } while (off < n);
    uint8_t d[8] = { 0 };
    memcpy(d, &n, 4); d[4] = count; d[5] = start; d[6] = d[7] = 0xFF;
    int r = write_item(ns, T_BLOB_IDX, k, CHUNK_ANY, d, 0, 0);
    if (r) return r;
    if (o >= 0) {                                              /* drop the previous version */
        for (int i = nidx - 1; i >= 0; i--) {
            if (idx[i].ns != ns || idx[i].hash != khash(k)) continue;
            bool old_data = idx[i].type == T_BLOB_DATA && (idx[i].chunk & 0x80) == (old.blob.start & 0x80);
            bool old_idx = idx[i].type == T_BLOB_IDX && idx[i].page == idx[o].page && idx[i].idx == idx[o].idx;
            if (old_data || old_idx) { item_t t; ent_rd(addr_of(idx[i].page, idx[i].idx), &t, 32);
                if (!strncmp(t.key, k, 16)) erase_ent(i); }
        }
    }
    return 0;
}

int nvs_get_blob(nvs_handle_t ns, const char *k, void *b, size_t *len)
{
    item_t ix;
    if (find(ns, T_BLOB_IDX, k, CHUNK_ANY, &ix) < 0) return NVS_NOT_FOUND;
    if (!b) { *len = ix.blob.size; return 0; }
    if (*len < ix.blob.size) { *len = ix.blob.size; return NVS_TOO_BIG; }
    size_t off = 0;
    for (int c = 0; c < ix.blob.count; c++) {
        item_t d;
        int i = find(ns, T_BLOB_DATA, k, ix.blob.start + c, &d);
        if (i < 0 || off + d.var.size > ix.blob.size) return NVS_ERR;
        if (ent_rd(addr_of(idx[i].page, idx[i].idx + 1), (uint8_t *)b + off, d.var.size)) return NVS_ERR;
        if (crc32_le(0xFFFFFFFF, (uint8_t *)b + off, d.var.size) != d.var.crc) return NVS_ERR;
        off += d.var.size;
    }
    *len = off;
    return off == ix.blob.size ? 0 : NVS_ERR;
}

int nvs_erase_key(nvs_handle_t ns, const char *k)
{
    if (find(ns, 0xFF, k, CHUNK_ANY, 0) < 0) return NVS_NOT_FOUND;
    return erase_all_of(ns, k, 0xFF);
}

int nvs_open(const char *name, nvs_handle_t *h)
{
    item_t it;
    if (!part) return NVS_ERR;
    if (find(0, T_U8, name, CHUNK_ANY, &it) >= 0) { *h = it.data[0]; return 0; }
    uint8_t used[32] = { 0 };                                  /* bitmap of taken indexes */
    for (int i = 0; i < nidx; i++)
        if (idx[i].ns == 0 && idx[i].type == T_U8) {
            item_t t; ent_rd(addr_of(idx[i].page, idx[i].idx), &t, 32);
            used[t.data[0] / 8] |= 1 << (t.data[0] % 8);
        }
    for (int n = 1; n < 255; n++)
        if (!(used[n / 8] & (1 << (n % 8)))) {
            uint8_t d[8]; memset(d, 0xFF, 8); d[0] = n;
            int r = write_item(0, T_U8, name, CHUNK_ANY, d, 0, 0);
            if (r) return r;
            *h = n;
            return 0;
        }
    return NVS_NO_SPACE;
}

int nvs_mount_secure(const char *label, const uint8_t key[64])
{
    static uint8_t k[64];
    memcpy(k, key, 64);
    aes_init();
    xkey = k;
    int r = nvs_mount_(label);
    if (r) xkey = NULL;
    return r;
}

int nvs_read_keys(const char *label, uint8_t key[64])
{
    const partition_t *kp = partition_find(PART_DATA, PART_SUB_NVS_KEYS, label);
    uint8_t b[68];
    if (!kp || partition_read(kp, 0, b, 68)) return NVS_NOT_FOUND;
    uint32_t c; memcpy(&c, b + 64, 4);
    if (c != crc32_le(0xFFFFFFFF, b, 64)) return NVS_ERR;     /* blank or damaged keys partition */
    memcpy(key, b, 64);
    return 0;
}

int nvs_mount(const char *label)
{
    xkey = NULL;
    return nvs_mount_(label);
}

static int nvs_mount_(const char *label)
{
    part = partition_find(PART_DATA, PART_SUB_NVS, label);
    if (!part) return NVS_ERR;
    npages = part->size / PAGE;
    if (npages > MAXPAGES) npages = MAXPAGES;
    nidx = 0; active = -1; max_seq = 0;
    for (int p = 0; p < npages; p++) {
        uint8_t h[64];
        if (partition_read(part, p * PAGE, h, 64)) return NVS_ERR;
        memcpy(&pg[p].state, h, 4);
        memcpy(&pg[p].seq, h + 4, 4);
        memcpy(pg[p].bitmap, h + 32, 32);
        uint32_t crc; memcpy(&crc, h + 28, 4);
        if (pg[p].state == ST_EMPTY) continue;
        if (crc != crc32_le(0xFFFFFFFF, h + 4, 24) || h[8] != VERSION) {   /* corrupt / foreign: reclaim */
            partition_erase(part, p * PAGE, PAGE);
            pg[p].state = ST_EMPTY;
            for (int k = 0; k < 8; k++) pg[p].bitmap[k] = ~0u;
            continue;
        }
        if (pg[p].seq > max_seq) max_seq = pg[p].seq;
        index_page(p);
        int last = -1;
        for (int e = 0; e < ENTRIES; e++) if (est(p, e) != 3) last = e;
        pg[p].next = last + 1;
        if (pg[p].state == ST_ACTIVE) active = p;
    }
    return 0;
}

int nvs_erase_all(void)
{
    if (!part) return NVS_ERR;
    if (partition_erase(part, 0, npages * PAGE)) return NVS_ERR;
    return nvs_mount_(part->label);
}

void nvs_stats(int *used, int *free_)
{
    int u = 0, f = 0;
    for (int p = 0; p < npages; p++)
        for (int e = 0; e < ENTRIES; e++) { int s = est(p, e); u += s == 2; f += s == 3; }
    if (used) *used = u;
    if (free_) *free_ = f;
}

/* ---- boot-time reader -------------------------------------------------------
 * No RAM index, no static state, ~200 B of stack: usable first thing after
 * reset, before nvs_mount(). One scan resolves namespaces, a second the keys;
 * duplicates resolve to the newest copy (higher page seq, then later entry). */
int nvs_boot_read(const char *label, nvs_boot_item_t *items, int n)
{
    const partition_t *p = partition_find(PART_DATA, PART_SUB_NVS, label);
    if (!p) return NVS_ERR;
    int pages = p->size / PAGE;
    uint32_t best_seq[n];                               /* per item: seq/index of the copy we hold */
    for (int i = 0; i < n; i++) { items[i].result = NVS_NOT_FOUND; items[i].ns_index = 0; best_seq[i] = 0; }
    for (int pass = 0; pass < 2; pass++)
        for (int pn = 0; pn < pages; pn++) {
            uint8_t h[64];
            if (partition_read(p, pn * PAGE, h, 64)) return NVS_ERR;
            uint32_t st, seq, c;
            memcpy(&st, h, 4); memcpy(&seq, h + 4, 4); memcpy(&c, h + 28, 4);
            if ((st != ST_ACTIVE && st != ST_FULL) || h[8] != VERSION || c != crc32_le(0xFFFFFFFF, h + 4, 24)) continue;
            for (int e = 0; e < ENTRIES; ) {
                if ((h[32 + e / 4] >> (2 * (e % 4)) & 3) != 2) { e++; continue; }
                item_t it;
                if (partition_read(p, addr_of(pn, e), &it, 32)) return NVS_ERR;
                int span = it.span ? it.span : 1;
                if (it.crc != item_crc(&it)) { e++; continue; }
                uint32_t rank = (seq + 1) * 128 + e;     /* newest wins */
                for (int i = 0; i < n; i++) {
                    nvs_boot_item_t *x = &items[i];
                    const char *want = pass ? x->key : x->ns;
                    if (strncmp(it.key, want, 16) || rank < best_seq[i]) continue;
                    if (!pass) {
                        if (it.ns == 0 && it.type == T_U8) { x->ns_index = it.data[0]; best_seq[i] = rank; }
                        continue;
                    }
                    if (!x->ns_index || it.ns != x->ns_index) continue;
                    if (it.type != x->type) { x->result = NVS_ERR; continue; }     /* key exists, other type */
                    if (it.type == T_SZ) {
                        if (it.var.size > x->len) { x->result = NVS_TOO_BIG; x->len = it.var.size; continue; }
                        if (partition_read(p, addr_of(pn, e + 1), x->str, it.var.size) ||
                            crc32_le(0xFFFFFFFF, x->str, it.var.size) != it.var.crc) { x->result = NVS_ERR; continue; }
                        x->len = it.var.size;
                    } else {
                        memcpy(&x->u64, it.data, 8);
                    }
                    x->result = NVS_OK;
                    best_seq[i] = rank;
                }
                e += span;
            }
            if (!pass && pn == pages - 1) for (int i = 0; i < n; i++) best_seq[i] = 0;   /* ranks restart for keys */
        }
    return 0;
}
