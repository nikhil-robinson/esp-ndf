/* SPIFFS, ESP-IDF default on-flash format.
 *
 * Block (4 KB) = 16 pages of 256 B. Page 0 = lookup: one u16 per page 1..15
 * (0xFFFF free, 0 deleted, id = data page, id|0x8000 = index page); bytes
 * 252/254 = block magic / erase count. Pages carry a 5 B header
 * {u16 id, u16 span, u8 flags}; flags are active-low (USED, FINAL, INDEX,
 * IXDELE, DELET). A file = index pages (span 0 = header with size + name +
 * 103 data page numbers, span k = 124 more each) + 251 B data pages.
 * Nothing is rewritten in place: a change writes a new page and marks the
 * old one deleted; GC moves a block's live pages out and erases it.
 *
 * Speed over IDF: the lookup pages live in RAM (2 B per page), so finding
 * free pages, files and index pages never re-reads the flash lookup tables. */
#include "spiffs.h"
#include "ndf_config.h"
#include "flash.h"
#include <string.h>

#define PG          256
#define PPB         16                  /* pages per block */
#define ENT         15                  /* usable pages per block (page 0 = lookup) */
#define DATA        251                 /* data bytes per page */
#define HDR_IX      49                  /* index header: 8 hdr + size + type + name + meta */
#define N_IX0       103                 /* data refs in the header index page */
#define N_IX        124                 /* data refs in other index pages */
#define NAME        32
#define IXF         0x8000
#define FREE        0xFFFF
#define DELETED     0
#define F_USED      0x01
#define F_FINAL     0x02
#define F_INDEX     0x04
#define F_IXDELE    0x40
#define F_DELET     0x80
#define FL_INDEX    0xF8                /* used + final + index */
#define FL_DATA     0xFC                /* used + final */
#define MAX_BLOCKS  NDF_SPIFFS_MAX_BLOCKS /* 4 KB blocks; 30 B of lookup mirror RAM each */
#define MAX_FD      NDF_SPIFFS_MAX_FILES
#define MAX_DIR     NDF_SPIFFS_MAX_DIRS

static const partition_t *part;
static uint32_t nblk, cur_blk;
static uint16_t lu[MAX_BLOCKS * ENT], max_ec;
static bool mounted;

typedef struct {
    bool used, wdirty, ixdirty;
    uint16_t id, ixpix;
    int flags;
    uint32_t size, pos;
    int32_t wspan, ixspan;              /* -1 = nothing cached */
    uint8_t wbuf[DATA], ix[PG];
} file_t;
static file_t fds[MAX_FD];
static struct { bool used; uint32_t next; char prefix[NAME]; size_t len; } dirs[MAX_DIR];

/* ---- page / lookup primitives ------------------------------------------ */

static inline uint32_t pix_of(uint32_t b, uint32_t e) { return b * PPB + 1 + e; }
static inline uint16_t *lu_of(uint32_t pix) { return &lu[(pix / PPB) * ENT + pix % PPB - 1]; }
static inline uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static int lu_set(uint32_t pix, uint16_t v)
{
    *lu_of(pix) = v;
    uint8_t b[2]; wr16(b, v);
    return partition_write(part, (pix / PPB) * FLASH_SECTOR + (pix % PPB - 1) * 2, b, 2);
}

static int page_delete(uint32_t pix)
{
    uint8_t f;
    if (partition_read(part, pix * PG + 4, &f, 1)) return FAT_EIO;
    f &= ~F_DELET;
    if (partition_write(part, pix * PG + 4, &f, 1)) return FAT_EIO;
    return lu_set(pix, DELETED) ? FAT_EIO : 0;
}

static uint16_t magic(uint32_t b) { return (uint16_t)(0x20140529 ^ PG ^ (nblk - b)); }

static int erase_block(uint32_t b)
{
    uint8_t t[4];
    wr16(t, magic(b)); wr16(t + 2, max_ec);
    if (++max_ec == FREE) max_ec = 0;
    if (partition_erase(part, b * FLASH_SECTOR, FLASH_SECTOR) ||
        partition_write(part, b * FLASH_SECTOR + PG - 4, t, 4)) return FAT_EIO;
    for (int e = 0; e < ENT; e++) lu[b * ENT + e] = FREE;
    return 0;
}

static bool blk_free(uint32_t b) { for (int e = 0; e < ENT; e++) if (lu[b * ENT + e] != FREE) return false; return true; }

static uint32_t free_blocks(void) { uint32_t n = 0; for (uint32_t b = 0; b < nblk; b++) n += blk_free(b); return n; }

/* A free page in a partly used block, else open an empty block; normal
 * writes keep 2 empty blocks in reserve (as IDF does), GC may use them. */
static int32_t alloc(bool gc, uint32_t skip)
{
    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t b = (cur_blk + i) % nblk;
        if (b == skip || blk_free(b)) continue;
        for (int e = 0; e < ENT; e++) if (lu[b * ENT + e] == FREE) { cur_blk = b; return pix_of(b, e); }
    }
    if (free_blocks() <= (gc ? 0u : 2u)) return -1;
    for (uint32_t b = 0; b < nblk; b++)
        if (b != skip && blk_free(b)) { cur_blk = b; return pix_of(b, 0); }
    return -1;
}

static int gc(void);

/* write a full page (header already in buf) to a new location */
static int32_t page_new(const uint8_t *buf, uint16_t luv)
{
    int32_t p = alloc(false, ~0u);
    for (int i = 0; p < 0 && i < 8; i++) {      /* each gc frees >= 1 page */
        if (gc()) return FAT_ENOSPC;
        p = alloc(false, ~0u);
    }
    if (p < 0) return FAT_ENOSPC;
    if (lu_set(p, luv) || partition_write(part, p * PG, buf, PG)) return FAT_EIO;
    return p;
}

/* index page of object id with span k, or -1 */
static int32_t find_ix(uint16_t id, uint32_t k)
{
    for (uint32_t i = 0; i < nblk * ENT; i++) {
        if (lu[i] != (id | IXF)) continue;
        uint32_t p = pix_of(i / ENT, i % ENT);
        uint8_t h[5];
        if (partition_read(part, p * PG, h, 5)) return -1;
        if (rd16(h + 2) == k && (h[4] & F_DELET) && !(h[4] & F_USED)) return p;
    }
    return -1;
}

static void ix_slot(uint32_t span, uint32_t *k, uint32_t *off)
{
    if (span < N_IX0) { *k = 0; *off = HDR_IX + 2 * span; }
    else { *k = 1 + (span - N_IX0) / N_IX; *off = 8 + 2 * ((span - N_IX0) % N_IX); }
}

/* ---- garbage collection -------------------------------------------------- */

/* data page of (id, span) moved old -> new: fix whichever index refers to it */
static int reindex(uint16_t id, uint32_t span, uint32_t old, uint32_t new)
{
    uint32_t k, off;
    ix_slot(span, &k, &off);
    for (int f = 0; f < MAX_FD; f++)
        if (fds[f].used && fds[f].id == id && fds[f].ixspan == (int32_t)k) {
            if (rd16(fds[f].ix + off) == old) { wr16(fds[f].ix + off, new); fds[f].ixdirty = true; }
        }
    int32_t q = find_ix(id, k);
    if (q < 0) return 0;                       /* only in a cache not yet written */
    uint8_t b[PG];
    if (partition_read(part, q * PG, b, PG)) return FAT_EIO;
    if (rd16(b + off) != old) return 0;
    wr16(b + off, new);
    int32_t n = alloc(true, q / PPB);
    if (n < 0 || lu_set(n, id | IXF) || partition_write(part, n * PG, b, PG)) return FAT_EIO;
    for (int f = 0; f < MAX_FD; f++) if (fds[f].used && fds[f].ixpix == q) fds[f].ixpix = n;
    return page_delete(q);
}

/* Reclaim the block with the most deleted pages: move live pages, erase. */
static int gc(void)
{
    int32_t victim = -1, best = 0;
    for (uint32_t b = 0; b < nblk; b++) {
        int d = 0;
        for (int e = 0; e < ENT; e++) d += lu[b * ENT + e] == DELETED;
        if (d > best) { best = d; victim = b; }
    }
    if (victim < 0) return FAT_ENOSPC;
    uint8_t buf[PG];
    for (int e = 0; e < ENT; e++) {
        uint32_t p = pix_of(victim, e);
        uint16_t v = *lu_of(p);
        if (v == FREE || v == DELETED) continue;
        int32_t n = alloc(true, victim);
        if (n < 0) return FAT_ENOSPC;
        if (partition_read(part, p * PG, buf, PG) || lu_set(n, v) || partition_write(part, n * PG, buf, PG))
            return FAT_EIO;
        if (v & IXF) { for (int f = 0; f < MAX_FD; f++) if (fds[f].used && fds[f].ixpix == p) fds[f].ixpix = n; }
        else if (reindex(v, rd16(buf + 2), p, n)) return FAT_EIO;
        if (page_delete(p)) return FAT_EIO;
    }
    if (cur_blk == (uint32_t)victim) cur_blk = (victim + 1) % nblk;
    return erase_block(victim);
}

/* ---- mount / format ------------------------------------------------------ */

int spiffs_format(const partition_t *p)
{
    if (!p || p->size / FLASH_SECTOR > MAX_BLOCKS) return FAT_EINVAL;
    for (int f = 0; f < MAX_FD; f++) if (fds[f].used) return FAT_EINVAL;
    part = p; nblk = p->size / FLASH_SECTOR; max_ec = 0;
    for (uint32_t b = 0; b < nblk; b++) if (erase_block(b)) return FAT_EIO;
    mounted = false;
    return 0;
}

int spiffs_mount(const partition_t *p)
{
    if (!p || p->size / FLASH_SECTOR > MAX_BLOCKS) return FAT_EINVAL;
    part = p; nblk = p->size / FLASH_SECTOR; cur_blk = 0;
    uint8_t pg[PG];
    int32_t unerased = -1;
    uint16_t ec_min = FREE, ec_max = 0;
    for (uint32_t b = 0; b < nblk; b++) {
        if (partition_read(p, b * FLASH_SECTOR, pg, PG)) return FAT_EIO;
        if (rd16(pg + PG - 4) != magic(b)) {
            if (unerased >= 0) return FAT_EINVAL;           /* not a SPIFFS volume */
            unerased = b;
        }
        uint16_t ec = rd16(pg + PG - 2);
        if (ec != FREE) { if (ec < ec_min) ec_min = ec; if (ec > ec_max) ec_max = ec; }
        for (int e = 0; e < ENT; e++) lu[b * ENT + e] = rd16(pg + 2 * e);
    }
    /* same erase-counter continuation rule as IDF (handles wrap) */
    max_ec = ec_min == 0 && ec_max == FREE ? 0 : ec_max - ec_min > FREE / 2 ? ec_min + 1 : ec_max + 1;
    if (unerased >= 0 && erase_block(unerased)) return FAT_EIO;   /* power lost mid-erase */
    memset(fds, 0, sizeof fds);
    memset(dirs, 0, sizeof dirs);
    mounted = true;
    return 0;
}

int spiffs_unmount(void)
{
    for (int f = 0; f < MAX_FD; f++) if (fds[f].used) spiffs_close(f);
    mounted = false;
    return 0;
}

/* ---- files ----------------------------------------------------------- */

/* header page of a live file by name -> pix, id, size */
static int32_t find_file(const char *name, uint16_t *id, uint32_t *size)
{
    uint8_t h[HDR_IX];
    for (uint32_t i = 0; i < nblk * ENT; i++) {
        if (lu[i] == FREE || lu[i] == DELETED || !(lu[i] & IXF)) continue;
        uint32_t p = pix_of(i / ENT, i % ENT);
        if (partition_read(part, p * PG, h, HDR_IX)) return -1;
        if (rd16(h + 2) != 0 || !(h[4] & F_DELET) || !(h[4] & F_IXDELE) || (h[4] & F_USED)) continue;
        if (strncmp((char *)h + 13, name, NAME)) continue;
        if (id) *id = lu[i] & ~IXF;
        if (size) *size = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
        return p;
    }
    return -1;
}

static uint16_t new_id(void)
{
    uint16_t m = 0;
    for (uint32_t i = 0; i < nblk * ENT; i++)
        if (lu[i] != FREE && lu[i] != DELETED && (lu[i] & ~IXF) > m) m = lu[i] & ~IXF;
    if (m < 0x7FFE) return m + 1;
    for (uint16_t id = 1; id < 0x7FFF; id++) {             /* ids exhausted at the top: reuse a gap */
        bool used = false;
        for (uint32_t i = 0; i < nblk * ENT && !used; i++) used = (lu[i] & ~IXF) == id && lu[i] != FREE;
        if (!used) return id;
    }
    return 0;
}

static void hdr(uint8_t *b, uint16_t id, uint16_t span, uint8_t fl)
{
    memset(b, 0xFF, PG);
    wr16(b, id); wr16(b + 2, span); b[4] = fl;
}

static int ix_flush(file_t *f)
{
    if (!f->ixdirty) return 0;
    if (f->ixspan == 0) { uint32_t s = f->size; f->ix[8] = s; f->ix[9] = s >> 8; f->ix[10] = s >> 16; f->ix[11] = s >> 24; }
    f->ixdirty = false;                         /* gc inside page_new must not see it dirty-stale */
    int32_t n = page_new(f->ix, f->id | IXF);
    if (n < 0) { f->ixdirty = true; return n; }
    uint16_t old = f->ixpix;                    /* read after page_new: gc may have moved it */
    f->ixpix = n;
    return old != FREE ? page_delete(old) : 0;
}

static int ix_load(file_t *f, uint32_t k)
{
    if (f->ixspan == (int32_t)k) return 0;
    int r = ix_flush(f);
    if (r) return r;
    int32_t p = find_ix(f->id, k);
    if (p >= 0) { if (partition_read(part, p * PG, f->ix, PG)) return FAT_EIO; f->ixpix = p; }
    else { hdr(f->ix, f->id | IXF, k, FL_INDEX); f->ixpix = FREE; }
    f->ixspan = k;
    return 0;
}

static int32_t data_pix(file_t *f, uint32_t span)
{
    uint32_t k, off;
    ix_slot(span, &k, &off);
    int r = ix_load(f, k);
    return r ? r : rd16(f->ix + off);
}

static int w_flush(file_t *f)
{
    if (!f->wdirty) return 0;
    uint8_t b[PG];
    hdr(b, f->id, f->wspan, FL_DATA);
    memcpy(b + 5, f->wbuf, DATA);
    int32_t n = page_new(b, f->id);
    if (n < 0) return n;
    uint32_t k, off;
    ix_slot(f->wspan, &k, &off);
    int r = ix_load(f, k);
    if (r) return r;
    uint16_t old = rd16(f->ix + off);           /* read after page_new: gc may have moved it */
    wr16(f->ix + off, n);
    f->ixdirty = true;
    f->wdirty = false;
    return old != FREE ? page_delete(old) : 0;
}

static int w_load(file_t *f, uint32_t span)
{
    if (f->wspan == (int32_t)span) return 0;
    int r = w_flush(f);
    if (r) return r;
    int32_t p = data_pix(f, span);
    if (p < 0) return p;
    if (p == FREE) memset(f->wbuf, 0xFF, DATA);
    else if (partition_read(part, p * PG + 5, f->wbuf, DATA)) return FAT_EIO;
    f->wspan = span;
    return 0;
}

static file_t *get(int fd) { return fd >= 0 && fd < MAX_FD && fds[fd].used ? &fds[fd] : NULL; }

int spiffs_unlink(const char *path)
{
    uint16_t id;
    int32_t h = find_file(path, &id, NULL);
    if (h < 0) return FAT_ENOENT;
    for (int f = 0; f < MAX_FD; f++) if (fds[f].used && fds[f].id == id) return FAT_EINVAL;
    uint8_t fl;                                  /* mark the file deleted first, like IDF */
    if (partition_read(part, h * PG + 4, &fl, 1)) return FAT_EIO;
    fl &= ~F_IXDELE;
    if (partition_write(part, h * PG + 4, &fl, 1)) return FAT_EIO;
    for (uint32_t i = 0; i < nblk * ENT; i++)
        if (lu[i] == id || lu[i] == (id | IXF))
            if (page_delete(pix_of(i / ENT, i % ENT))) return FAT_EIO;
    return 0;
}

int spiffs_open(const char *path, int flags)
{
    if (!mounted) return FAT_EINVAL;
    size_t len = strlen(path);
    if (len == 0 || len >= NAME) return FAT_EINVAL;
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return FAT_EMFILE;
    file_t *f = &fds[fd];
    uint16_t id; uint32_t size = 0;
    int32_t h = find_file(path, &id, &size);
    if (h >= 0 && (flags & FAT_TRUNC) && (flags & 3)) { int r = spiffs_unlink(path); if (r) return r; h = -1; }
    if (h < 0) {
        if (!(flags & FAT_CREAT)) return FAT_ENOENT;
        id = new_id();
        if (!id) return FAT_ENOSPC;
        uint8_t b[PG];
        hdr(b, id | IXF, 0, FL_INDEX);
        memset(b + 8, 0, 4);                     /* size 0 */
        b[12] = 1;                               /* type: file */
        memset(b + 13, 0, NAME + 4);             /* name + meta (mtime 0) */
        memcpy(b + 13, path, len);
        h = page_new(b, id | IXF);
        if (h < 0) return h;
        size = 0;
    }
    *f = (file_t){ .used = true, .id = id, .flags = flags, .size = size, .wspan = -1, .ixspan = -1, .ixpix = FREE };
    f->pos = flags & FAT_APPEND ? size : 0;
    return fd;
}

int spiffs_read(int fd, void *buf, size_t n)
{
    file_t *f = get(fd);
    if (!f || (f->flags & 3) == FAT_WRONLY) return FAT_EINVAL;
    if (f->pos >= f->size) return 0;
    if (n > f->size - f->pos) n = f->size - f->pos;
    uint8_t *d = buf;
    for (size_t left = n; left; ) {
        uint32_t span = f->pos / DATA, off = f->pos % DATA, c = DATA - off;
        if (c > left) c = left;
        if (f->wspan == (int32_t)span) memcpy(d, f->wbuf + off, c);
        else {
            int32_t p = data_pix(f, span);
            if (p < 0) return p;
            if (p == FREE) memset(d, 0xFF, c);
            else if (partition_read(part, p * PG + 5 + off, d, c)) return FAT_EIO;
        }
        d += c; f->pos += c; left -= c;
    }
    return n;
}

int spiffs_write(int fd, const void *buf, size_t n)
{
    file_t *f = get(fd);
    if (!f || (f->flags & 3) == FAT_RDONLY) return FAT_EINVAL;
    if (f->flags & FAT_APPEND) f->pos = f->size;
    const uint8_t *s = buf;
    for (size_t left = n; left; ) {
        uint32_t span = f->pos / DATA, off = f->pos % DATA, c = DATA - off;
        if (c > left) c = left;
        int r = w_load(f, span);
        if (r) return n - left ? (int)(n - left) : r;
        memcpy(f->wbuf + off, s, c);
        f->wdirty = true;
        s += c; f->pos += c; left -= c;
        if (f->pos > f->size) f->size = f->pos;
    }
    return n;
}

int spiffs_seek(int fd, int32_t off, int whence)
{
    file_t *f = get(fd);
    if (!f) return FAT_EINVAL;
    int32_t base = whence == FAT_SEEK_SET ? 0 : whence == FAT_SEEK_CUR ? (int32_t)f->pos : (int32_t)f->size;
    if (base + off < 0 || (uint32_t)(base + off) > f->size) return FAT_EINVAL;
    f->pos = base + off;
    return f->pos;
}

int spiffs_sync(int fd)
{
    file_t *f = get(fd);
    if (!f) return FAT_EINVAL;
    if ((f->flags & 3) == FAT_RDONLY) return 0;
    int r = w_flush(f);
    if (!r) r = ix_load(f, 0);                   /* size lives in the header page */
    if (!r) {
        uint32_t s = f->ix[8] | f->ix[9] << 8 | f->ix[10] << 16 | (uint32_t)f->ix[11] << 24;
        if (s != f->size) f->ixdirty = true;
        r = ix_flush(f);
    }
    return r;
}

int spiffs_close(int fd)
{
    int r = spiffs_sync(fd);
    if (get(fd)) fds[fd].used = false;
    return r;
}

int spiffs_stat(const char *path, uint32_t *size, bool *is_dir)
{
    if (is_dir) *is_dir = !strcmp(path, "/");
    if (!strcmp(path, "/")) { if (size) *size = 0; return 0; }
    return find_file(path, NULL, size) < 0 ? FAT_ENOENT : 0;
}

int spiffs_opendir(const char *path)
{
    for (int d = 0; d < MAX_DIR; d++)
        if (!dirs[d].used) {
            size_t len = strlen(path);
            while (len && path[len - 1] == '/') len--;
            if (len >= NAME) return FAT_EINVAL;
            memcpy(dirs[d].prefix, path, len); dirs[d].prefix[len] = 0;
            dirs[d].len = len; dirs[d].next = 0; dirs[d].used = true;
            return d;
        }
    return FAT_EMFILE;
}

int spiffs_readdir(int dd, char *name, uint32_t *size, bool *is_dir)
{
    if (dd < 0 || dd >= MAX_DIR || !dirs[dd].used) return FAT_EINVAL;
    uint8_t h[HDR_IX];
    while (dirs[dd].next < nblk * ENT) {
        uint32_t i = dirs[dd].next++;
        if (lu[i] == FREE || lu[i] == DELETED || !(lu[i] & IXF)) continue;
        if (partition_read(part, pix_of(i / ENT, i % ENT) * PG, h, HDR_IX)) return FAT_EIO;
        if (rd16(h + 2) != 0 || !(h[4] & F_DELET) || !(h[4] & F_IXDELE)) continue;
        const char *n = (char *)h + 13;
        size_t pl = dirs[dd].len;
        if (strncmp(n, dirs[dd].prefix, pl) || n[pl] != '/') continue;
        strncpy(name, n + pl + 1, NAME); name[NAME - 1] = 0;
        if (size) *size = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
        if (is_dir) *is_dir = false;
        return 1;
    }
    return 0;
}

int spiffs_closedir(int dd)
{
    if (dd < 0 || dd >= MAX_DIR) return FAT_EINVAL;
    dirs[dd].used = false;
    return 0;
}

void spiffs_info(uint32_t *total, uint32_t *used)
{
    uint32_t u = 0;
    for (uint32_t i = 0; i < nblk * ENT; i++) u += lu[i] != FREE && lu[i] != DELETED;
    if (total) *total = nblk * ENT * PG;
    if (used) *used = u * PG;
}
