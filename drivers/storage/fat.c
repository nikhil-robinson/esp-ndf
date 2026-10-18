/* FAT12/16/32 + VFAT long names. Two one-sector caches (FAT, data/dir) with
 * write-back; FAT sectors are mirrored to every FAT copy when flushed. */
#include "fat.h"
#include <string.h>

#define MAXSS    4096
#define MAXFILES 8
#define MAXDIRS  4
#define DELETED  0xE5
#define A_RO     0x01
#define A_VOL    0x08
#define A_DIR    0x10
#define A_ARCH   0x20
#define A_LFN    0x0F

/* ---- volume + caches ------------------------------------------------------ */
static struct {
    bdev_t *bd;
    uint32_t bps, spc, rsvd, nfats, root_ents, fatsz, totsec;
    uint32_t fat_start, root_start, root_secs, data_start, nclus, root_clus, hint;
    uint32_t repaired;                                  /* FAT sectors rebuilt from the mirror at mount */
    int bits;
    bool mounted;
} v;

typedef struct { uint8_t buf[MAXSS] __attribute__((aligned(4))); uint32_t sec; bool valid, dirty, is_fat; } cache_t;
static cache_t cfat, cdat;
static void pcache_clear(void);

static int flush(cache_t *c)
{
    if (!c->valid || !c->dirty) return 0;
    if (c->is_fat) {
        for (uint32_t k = 0; k < v.nfats; k++)
            if (v.bd->write(v.bd, c->sec + k * v.fatsz, c->buf, 1)) return FAT_EIO;
    } else if (v.bd->write(v.bd, c->sec, c->buf, 1)) return FAT_EIO;
    c->dirty = false;
    return 0;
}

static uint8_t *get(cache_t *c, uint32_t sec)
{
    if (c->valid && c->sec == sec) return c->buf;
    if (flush(c)) return 0;
    if (v.bd->read(v.bd, sec, c->buf, 1)) { c->valid = false; return 0; }
    c->sec = sec; c->valid = true; c->is_fat = c == &cfat;
    return c->buf;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
static void wr32(uint8_t *p, uint32_t x) { p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24; }

/* ---- FAT table --------------------------------------------------------------- */
static uint32_t eoc(void) { return v.bits == 12 ? 0xFFF : v.bits == 16 ? 0xFFFF : 0x0FFFFFFF; }
static bool is_eoc(uint32_t x) { return x >= (v.bits == 12 ? 0xFF8u : v.bits == 16 ? 0xFFF8u : 0x0FFFFFF8u); }

static uint32_t fat_get(uint32_t c)
{
    if (v.bits == 12) {
        uint32_t off = c + c / 2, s = v.fat_start + off / v.bps;
        uint8_t *b = get(&cfat, s); if (!b) return 0x0FFFFFFF;
        uint32_t lo = b[off % v.bps];
        b = get(&cfat, v.fat_start + (off + 1) / v.bps); if (!b) return 0x0FFFFFFF;
        uint32_t x = lo | b[(off + 1) % v.bps] << 8;
        return c & 1 ? x >> 4 : x & 0xFFF;
    }
    uint32_t w = v.bits / 8, off = c * w;
    uint8_t *b = get(&cfat, v.fat_start + off / v.bps); if (!b) return 0x0FFFFFFF;
    return w == 2 ? rd16(b + off % v.bps) : rd32(b + off % v.bps) & 0x0FFFFFFF;
}

static int fat_set(uint32_t c, uint32_t x)
{
    if (v.bits == 12) {
        uint32_t off = c + c / 2;
        for (int k = 0; k < 2; k++) {
            uint8_t *b = get(&cfat, v.fat_start + (off + k) / v.bps); if (!b) return FAT_EIO;
            uint8_t *p = &b[(off + k) % v.bps];
            if (c & 1) *p = k == 0 ? (*p & 0x0F) | (x << 4 & 0xF0) : x >> 4;
            else       *p = k == 0 ? x & 0xFF : (*p & 0xF0) | (x >> 8 & 0x0F);
            cfat.dirty = true;
        }
        return 0;
    }
    uint32_t w = v.bits / 8, off = c * w;
    uint8_t *b = get(&cfat, v.fat_start + off / v.bps); if (!b) return FAT_EIO;
    if (w == 2) wr16(b + off % v.bps, x);
    else wr32(b + off % v.bps, (rd32(b + off % v.bps) & 0xF0000000) | (x & 0x0FFFFFFF));
    cfat.dirty = true;
    return 0;
}

static uint32_t clus_sec(uint32_t c) { return v.data_start + (c - 2) * v.spc; }

static int zero_clus(uint32_t c)
{
    if (flush(&cdat)) return FAT_EIO;
    memset(cdat.buf, 0, v.bps);
    for (uint32_t i = 0; i < v.spc; i++) if (v.bd->write(v.bd, clus_sec(c) + i, cdat.buf, 1)) return FAT_EIO;
    cdat.valid = false;
    return 0;
}

/* allocate a free cluster, link it after `prev` (0 = start of a chain) */
static uint32_t alloc(uint32_t prev, bool zero)
{
    for (uint32_t n = 0; n < v.nclus; n++) {
        uint32_t c = 2 + (v.hint - 2 + n) % v.nclus;
        if (fat_get(c) != 0) continue;
        if (fat_set(c, eoc())) return 0;
        if (prev && fat_set(prev, c)) return 0;
        v.hint = c;
        if (zero && zero_clus(c)) return 0;
        return c;
    }
    return 0;
}

static int free_chain(uint32_t c)
{
    while (c >= 2 && c < v.nclus + 2) {
        uint32_t n = fat_get(c);
        if (fat_set(c, 0)) return FAT_EIO;
        if (is_eoc(n) || n < 2) break;
        c = n;
    }
    return 0;
}

/* ---- directories ------------------------------------------------------------ */
/* entry `i` of the directory starting at cluster `dc` (0 = FAT12/16 fixed root).
 * Returns a pointer into the data cache, NULL past the end (extends if grow). */
/* sector holding entry i of directory dc, without reading it (0 = none) */
static uint32_t dent_sec(uint32_t dc, uint32_t i)
{
    uint32_t eps = v.bps / 32;
    if (dc == 0) return i < v.root_ents ? v.root_start + i / eps : 0;
    uint32_t per = eps * v.spc, c = dc, k = i / per;
    while (k--) {
        uint32_t n = fat_get(c);
        if (is_eoc(n) || n < 2) return 0;
        c = n;
    }
    return clus_sec(c) + (i % per) / eps;
}

static uint8_t *dent(uint32_t dc, uint32_t i, bool grow, uint32_t *sec_out)
{
    uint32_t eps = v.bps / 32, s;
    if (dc == 0) {
        if (i >= v.root_ents) return 0;
        s = v.root_start + i / eps;
    } else {
        uint32_t per = eps * v.spc, c = dc, k = i / per;
        while (k--) {
            uint32_t n = fat_get(c);
            if (is_eoc(n) || n < 2) {
                if (!grow) return 0;
                n = alloc(c, true);
                if (!n) return 0;
            }
            c = n;
        }
        s = clus_sec(c) + (i % per) / eps;
    }
    uint8_t *b = get(&cdat, s);
    if (sec_out) *sec_out = s;
    return b ? b + (i % eps) * 32 : 0;
}

static uint32_t first_clus(const uint8_t *e) { return rd16(e + 26) | (v.bits == 32 ? (uint32_t)rd16(e + 20) << 16 : 0); }
static void set_first(uint8_t *e, uint32_t c) { wr16(e + 26, c); wr16(e + 20, v.bits == 32 ? c >> 16 : 0); }

static uint8_t sfn_sum(const uint8_t *n) { uint8_t s = 0; for (int i = 0; i < 11; i++) s = ((s & 1) << 7) + (s >> 1) + n[i]; return s; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) if (lower(*a++) != lower(*b++)) return false;
    return !*a && !*b;
}

static void sfn_to_str(const uint8_t *e, char *out)
{
    int k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[k++] = (e[12] & 0x08) ? lower(e[i]) : e[i];
    if (e[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[k++] = (e[12] & 0x10) ? lower(e[i]) : e[i];
    }
    out[k] = 0;
    if (out[0] == 0x05) out[0] = (char)0xE5;
}

/* Walk directory entries. For each live entry, calls back with its long (or
 * short) name; stops when cb returns non-zero. pos receives the SFN index and
 * lfn_start the first LFN entry index. */
typedef int (*walk_fn)(const char *name, uint8_t *e, uint32_t idx, uint32_t lfn_start, void *ctx);

static int walk(uint32_t dc, uint32_t from, walk_fn fn, void *ctx, uint32_t *next)
{
    char lname[256]; int lok = 0; uint8_t lsum = 0; uint32_t lstart = 0;
    for (uint32_t i = from;; i++) {
        uint8_t *e = dent(dc, i, false, 0);
        if (!e || e[0] == 0) { if (next) *next = i; return 0; }
        if (e[0] == DELETED) { lok = 0; continue; }
        if (e[11] == A_LFN) {
            int ord = e[0] & 0x3F;
            if (e[0] & 0x40) { lok = ord; lsum = e[13]; lstart = i; memset(lname, 0, sizeof lname); }
            else if (ord != lok - 1 || e[13] != lsum) { lok = 0; continue; }
            else lok = ord;
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) {
                uint16_t ch = rd16(e + pos[k]);
                int at = (ord - 1) * 13 + k;
                if (at < 255 && ch && ch != 0xFFFF) lname[at] = ch < 0x80 ? ch : '?';
            }
            continue;
        }
        if (e[11] & A_VOL) { lok = 0; continue; }
        char name[256];
        bool has_l = lok == 1 && sfn_sum(e) == lsum;
        if (has_l) strcpy(name, lname); else sfn_to_str(e, name);
        uint32_t ls = has_l ? lstart : i;
        lok = 0;
        int r = fn(name, e, i, ls, ctx);
        if (r) { if (next) *next = i + 1; return r; }
    }
}

typedef struct { const char *want; uint32_t idx, lfn; uint8_t ent[32]; } find_t;
static int find_cb(const char *name, uint8_t *e, uint32_t idx, uint32_t ls, void *ctx)
{
    find_t *f = ctx;
    if (!name_eq(name, f->want)) return 0;
    f->idx = idx; f->lfn = ls; memcpy(f->ent, e, 32);
    return 1;
}

static uint32_t root_dc(void) { return v.bits == 32 ? v.root_clus : 0; }

/* resolve path: parent dir cluster + last component; optionally the entry */
/* Path lookup cache: repeated opens/stats of the same path skip the
 * directory walk (one 4 KB sector read per directory level). Any directory
 * change (create, entry update, unlink, mkdir, format, mount) empties it. */
#define PCACHE 4
static struct { char path[64]; uint32_t parent; uint16_t last_off; find_t f; bool valid; } pcache[PCACHE];
static uint8_t pcache_next;
static void pcache_clear(void) { for (int i = 0; i < PCACHE; i++) pcache[i].valid = false; }

static int resolve_walk(const char *path, uint32_t *parent, const char **last, find_t *f);

static int resolve(const char *path, uint32_t *parent, const char **last, find_t *f)
{
    size_t len = strnlen(path, sizeof pcache[0].path);
    if (f && len < sizeof pcache[0].path) {
        for (int i = 0; i < PCACHE; i++)
            if (pcache[i].valid && !strcmp(pcache[i].path, path)) {
                *parent = pcache[i].parent;
                *last = path + pcache[i].last_off;
                *f = pcache[i].f;
                f->want = *last;
                return 0;
            }
    }
    int r = resolve_walk(path, parent, last, f);
    if (!r && f && len < sizeof pcache[0].path) {
        int i = pcache_next;
        memcpy(pcache[i].path, path, len + 1);
        pcache[i].parent = *parent;
        pcache[i].last_off = *last - path;
        pcache[i].f = *f;
        pcache[i].valid = true;
        pcache_next = (i + 1) % PCACHE;
    }
    return r;
}

static int resolve_walk(const char *path, uint32_t *parent, const char **last, find_t *f)
{
    uint32_t dc = root_dc();
    while (*path == '/') path++;
    for (;;) {
        const char *slash = strchr(path, '/');
        if (!slash) break;
        char comp[256]; size_t n = slash - path;
        if (!n || n > 255) return FAT_EINVAL;
        memcpy(comp, path, n); comp[n] = 0;
        find_t ff = { .want = comp };
        if (walk(dc, 0, find_cb, &ff, 0) != 1) return FAT_ENOENT;
        if (!(ff.ent[11] & A_DIR)) return FAT_ENOTDIR;
        dc = first_clus(ff.ent);
        if (!dc) dc = root_dc();
        path = slash + 1;
    }
    *parent = dc; *last = path;
    if (!f) return 0;
    if (!*path) return FAT_EINVAL;
    f->want = path;
    return walk(dc, 0, find_cb, f, 0) == 1 ? 0 : FAT_ENOENT;
}

static bool valid_83(const char *n)
{
    int base = 0, ext = -1;
    for (const char *p = n; *p; p++) {
        if (*p == '.') { if (ext >= 0 || p == n) return false; ext = 0; continue; }
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_-~!#$%&'(){}^@`", *p))) return false;
        if (ext >= 0) { if (++ext > 3) return false; } else if (++base > 8) return false;
    }
    return base > 0;
}

static void make_sfn(const char *n, uint8_t out[11], int tail)
{
    memset(out, ' ', 11);
    const char *dot = strrchr(n, '.');
    int k = 0;
    for (const char *p = n; *p && p != dot && k < (tail ? 6 : 8); p++) {
        int c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out[k++] = c;
    }
    if (!k) out[k++] = '_';
    if (tail) { out[k++] = '~'; out[k++] = '0' + tail % 10; }
    if (dot) for (int i = 0, j = 8; dot[1 + i] && i < 3; i++) {
        int c = dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[j++] = c;
    }
}

typedef struct { const uint8_t *sfn; bool hit; } sfn_t;
static int sfn_cb(const char *name, uint8_t *e, uint32_t idx, uint32_t ls, void *ctx)
{
    (void)name; (void)idx; (void)ls;
    sfn_t *s = ctx;
    if (!memcmp(e, s->sfn, 11)) { s->hit = true; return 1; }
    return 0;
}

/* create a directory entry (LFN + SFN) for name in dc; returns SFN index */
static int create_entry(uint32_t dc, const char *name, uint8_t attr, uint32_t first, uint32_t *idx_out)
{
    pcache_clear();
    size_t len = strlen(name);
    if (!len || len > 255) return FAT_EINVAL;
    uint8_t sfn[11];
    int nl = 0;
    if (valid_83(name)) make_sfn(name, sfn, 0);
    else {
        nl = (len + 12) / 13;
        for (int t = 1; t < 10; t++) {                         /* unique NAME~n */
            make_sfn(name, sfn, t);
            sfn_t s = { sfn, false };
            walk(dc, 0, sfn_cb, &s, 0);
            if (!s.hit) break;
            if (t == 9) return FAT_EEXIST;
        }
    }
    int need = nl + 1;
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0;; i++) {                                /* find `need` free slots */
        uint8_t *e = dent(dc, i, true, 0);
        if (!e) return FAT_ENOSPC;
        if (e[0] == 0 || e[0] == DELETED) { if (!run) start = i; if (++run == (uint32_t)need) break; }
        else run = 0;
    }
    uint8_t sum = sfn_sum(sfn);
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = 0; k < nl; k++) {                              /* LFN entries, last part first */
        int ord = nl - k;
        uint8_t *e = dent(dc, start + k, true, 0);
        memset(e, 0, 32);
        e[0] = ord | (k == 0 ? 0x40 : 0);
        e[11] = A_LFN; e[13] = sum;
        for (int j = 0; j < 13; j++) {
            int at = (ord - 1) * 13 + j;
            uint16_t ch = at < (int)len ? (uint8_t)name[at] : at == (int)len ? 0 : 0xFFFF;
            wr16(e + pos[j], ch);
        }
        cdat.dirty = true;
    }
    uint8_t *e = dent(dc, start + nl, true, 0);
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    set_first(e, first);
    wr16(e + 24, (2024 - 1980) << 9 | 1 << 5 | 1);              /* date: 2024-01-01 */
    cdat.dirty = true;
    if (idx_out) *idx_out = start + nl;
    return 0;
}

/* ---- files ------------------------------------------------------------------ */
typedef struct {
    bool used;
    uint32_t dir_sec, dir_off, first, size, pos, cur, cur_i;
    int flags;
} file_t;
static file_t files[MAXFILES];

static int sync_entry(file_t *f)
{
    pcache_clear();
    uint8_t *b = get(&cdat, f->dir_sec);
    if (!b) return FAT_EIO;
    wr32(b + f->dir_off + 28, f->size);
    set_first(b + f->dir_off, f->first);
    cdat.dirty = true;
    return 0;
}

int fat_open(const char *path, int flags)
{
    if (!v.mounted) return FAT_EINVAL;
    int fd = -1;
    for (int i = 0; i < MAXFILES; i++) if (!files[i].used) { fd = i; break; }
    if (fd < 0) return FAT_EMFILE;
    uint32_t dc; const char *last; find_t f;
    int r = resolve(path, &dc, &last, &f);
    if (r == FAT_ENOENT && (flags & FAT_CREAT) && *last) {
        uint32_t idx;
        if ((r = create_entry(dc, last, A_ARCH, 0, &idx))) return r;
        if ((r = resolve(path, &dc, &last, &f))) return r;
    } else if (r) return r;
    if (f.ent[11] & A_DIR) return FAT_EISDIR;
    file_t *h = &files[fd];
    *h = (file_t){ .used = true, .first = first_clus(f.ent), .size = rd32(f.ent + 28), .flags = flags };
    h->dir_sec = dent_sec(dc, f.idx);            /* no directory read: the path cache already had the entry */
    h->dir_off = (f.idx % (v.bps / 32)) * 32;
    if ((flags & FAT_TRUNC) && (flags & 3)) {
        free_chain(h->first);
        h->first = 0; h->size = 0;
        sync_entry(h);
    }
    if (flags & FAT_APPEND) h->pos = h->size;
    return fd;
}

/* cluster holding byte `pos` (allocating when grow); 0 if none */
static uint32_t clus_at(file_t *h, uint32_t pos, bool grow)
{
    uint32_t cb = v.bps * v.spc, want = pos / cb;
    if (!h->first) {
        if (!grow) return 0;
        if (!(h->first = alloc(0, false))) return 0;
        h->cur = h->first; h->cur_i = 0;
    }
    if (!h->cur || want < h->cur_i) { h->cur = h->first; h->cur_i = 0; }
    while (h->cur_i < want) {
        uint32_t n = fat_get(h->cur);
        if (is_eoc(n) || n < 2) {
            if (!grow) return 0;
            if (!(n = alloc(h->cur, false))) return 0;
        }
        h->cur = n; h->cur_i++;
    }
    return h->cur;
}

int fat_read(int fd, void *buf, size_t n)
{
    if (fd < 0 || fd >= MAXFILES || !files[fd].used) return FAT_EINVAL;
    file_t *h = &files[fd];
    uint8_t *out = buf;
    size_t done = 0;
    while (done < n && h->pos < h->size) {
        uint32_t c = clus_at(h, h->pos, false);
        if (!c) return FAT_EIO;
        uint32_t in_c = h->pos % (v.bps * v.spc), sec = clus_sec(c) + in_c / v.bps, off = in_c % v.bps;
        uint32_t k = v.bps - off;
        if (k > n - done) k = n - done;
        if (k > h->size - h->pos) k = h->size - h->pos;
        uint8_t *b = get(&cdat, sec);
        if (!b) return FAT_EIO;
        memcpy(out + done, b + off, k);
        done += k; h->pos += k;
    }
    return done;
}

int fat_write(int fd, const void *buf, size_t n)
{
    if (fd < 0 || fd >= MAXFILES || !files[fd].used || !(files[fd].flags & 3)) return FAT_EINVAL;
    file_t *h = &files[fd];
    if (h->flags & FAT_APPEND) h->pos = h->size;
    const uint8_t *in = buf;
    size_t done = 0;
    while (done < n) {
        uint32_t c = clus_at(h, h->pos, true);
        if (!c) { sync_entry(h); return done ? (int)done : FAT_ENOSPC; }
        uint32_t in_c = h->pos % (v.bps * v.spc), sec = clus_sec(c) + in_c / v.bps, off = in_c % v.bps;
        uint32_t k = v.bps - off;
        if (k > n - done) k = n - done;
        uint8_t *b;
        if (off == 0 && k == v.bps && h->pos >= h->size) {     /* whole new sector: no read */
            if (flush(&cdat)) return FAT_EIO;
            cdat.sec = sec; cdat.valid = true; cdat.is_fat = false; b = cdat.buf;
        } else if (!(b = get(&cdat, sec))) return FAT_EIO;
        memcpy(b + off, in + done, k);
        cdat.dirty = true;
        done += k; h->pos += k;
        if (h->pos > h->size) h->size = h->pos;
    }
    return sync_entry(h) ? FAT_EIO : (int)done;
}

int fat_seek(int fd, int32_t off, int whence)
{
    if (fd < 0 || fd >= MAXFILES || !files[fd].used) return FAT_EINVAL;
    file_t *h = &files[fd];
    int64_t p = whence == FAT_SEEK_SET ? off : whence == FAT_SEEK_CUR ? (int64_t)h->pos + off : (int64_t)h->size + off;
    if (p < 0 || p > h->size) return FAT_EINVAL;               /* no holes */
    h->pos = p;
    return p;
}

int fat_close(int fd)
{
    if (fd < 0 || fd >= MAXFILES || !files[fd].used) return FAT_EINVAL;
    int r = (files[fd].flags & 3) ? sync_entry(&files[fd]) : 0;
    files[fd].used = false;
    return r ? r : fat_sync();
}

/* ---- namespace ops ------------------------------------------------------------- */
typedef struct { int n; } cnt_t;
static int count_cb(const char *name, uint8_t *e, uint32_t i, uint32_t l, void *ctx)
{
    (void)e; (void)i; (void)l;
    if (strcmp(name, ".") && strcmp(name, "..")) ((cnt_t *)ctx)->n++;
    return 0;
}

int fat_unlink(const char *path)
{
    pcache_clear();
    uint32_t dc; const char *last; find_t f;
    int r = resolve(path, &dc, &last, &f);
    if (r) return r;
    if (f.ent[11] & A_DIR) {
        cnt_t c = { 0 };
        walk(first_clus(f.ent), 0, count_cb, &c, 0);
        if (c.n) return FAT_ENOTEMPTY;
    }
    for (uint32_t i = f.lfn; i <= f.idx; i++) {                /* LFN entries + SFN */
        uint8_t *e = dent(dc, i, false, 0);
        if (!e) return FAT_EIO;
        e[0] = DELETED; cdat.dirty = true;
    }
    free_chain(first_clus(f.ent));
    return fat_sync();
}

int fat_mkdir(const char *path)
{
    pcache_clear();
    uint32_t dc; const char *last; find_t f;
    int r = resolve(path, &dc, &last, &f);
    if (!r) return FAT_EEXIST;
    if (r != FAT_ENOENT) return r;
    uint32_t c = alloc(0, true);
    if (!c) return FAT_ENOSPC;
    if ((r = create_entry(dc, last, A_DIR, c, 0))) return r;
    uint8_t *e = dent(c, 0, false, 0);
    memset(e, ' ', 11); e[0] = '.'; e[11] = A_DIR; set_first(e, c);
    e = dent(c, 1, false, 0);
    memset(e, ' ', 11); e[0] = e[1] = '.'; e[11] = A_DIR; set_first(e, dc == root_dc() ? 0 : dc);
    cdat.dirty = true;
    return fat_sync();
}

int fat_stat(const char *path, uint32_t *size, bool *dir)
{
    uint32_t dc; const char *last; find_t f;
    if (!strcmp(path, "/")) { if (size) *size = 0; if (dir) *dir = true; return 0; }
    int r = resolve(path, &dc, &last, &f);
    if (r) return r;
    if (size) *size = rd32(f.ent + 28);
    if (dir) *dir = f.ent[11] & A_DIR;
    return 0;
}

/* ---- directory listing ------------------------------------------------------------ */
typedef struct { bool used; uint32_t dc, next; } dir_t;
static dir_t dirs[MAXDIRS];

int fat_opendir(const char *path)
{
    uint32_t dc = root_dc();
    if (strcmp(path, "/") && *path) {
        uint32_t p; const char *last; find_t f;
        int r = resolve(path, &p, &last, &f);
        if (r) return r;
        if (!(f.ent[11] & A_DIR)) return FAT_ENOTDIR;
        dc = first_clus(f.ent);
    }
    for (int i = 0; i < MAXDIRS; i++) if (!dirs[i].used) { dirs[i] = (dir_t){ true, dc, 0 }; return i; }
    return FAT_EMFILE;
}

typedef struct { char *name; uint32_t *size; bool *dir; } rd_t;
static int rd_cb(const char *name, uint8_t *e, uint32_t i, uint32_t l, void *ctx)
{
    (void)i; (void)l;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    rd_t *r = ctx;
    strcpy(r->name, name);
    if (r->size) *r->size = rd32(e + 28);
    if (r->dir) *r->dir = e[11] & A_DIR;
    return 1;
}

int fat_readdir(int dd, char *name, uint32_t *size, bool *dir)
{
    if (dd < 0 || dd >= MAXDIRS || !dirs[dd].used) return FAT_EINVAL;
    rd_t r = { name, size, dir };
    return walk(dirs[dd].dc, dirs[dd].next, rd_cb, &r, &dirs[dd].next);
}

int fat_closedir(int dd) { if (dd >= 0 && dd < MAXDIRS) dirs[dd].used = false; return 0; }

/* ---- volume ----------------------------------------------------------------------- */
int fat_sync(void)
{
    if (flush(&cdat) || flush(&cfat)) return FAT_EIO;
    return v.bd->sync ? v.bd->sync(v.bd) : 0;
}

int fat_mount(bdev_t *b)
{
    pcache_clear();
    v.bd = b; v.mounted = false; v.repaired = 0;
    cfat.valid = cdat.valid = false;
    if (b->sector_size > MAXSS) return FAT_EINVAL;
    uint8_t *s = get(&cdat, 0);
    if (!s || s[510] != 0x55 || s[511] != 0xAA) return FAT_EINVAL;
    v.bps = rd16(s + 11); v.spc = s[13]; v.rsvd = rd16(s + 14); v.nfats = s[16];
    v.root_ents = rd16(s + 17);
    v.totsec = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    v.fatsz = rd16(s + 22) ? rd16(s + 22) : rd32(s + 36);
    if (v.bps != b->sector_size || !v.spc || !v.nfats || !v.fatsz) return FAT_EINVAL;
    v.fat_start = v.rsvd;
    v.root_start = v.rsvd + v.nfats * v.fatsz;
    v.root_secs = (v.root_ents * 32 + v.bps - 1) / v.bps;
    v.data_start = v.root_start + v.root_secs;
    v.nclus = (v.totsec - v.data_start) / v.spc;
    v.bits = v.nclus < 4085 ? 12 : v.nclus < 65525 ? 16 : 32;
    v.root_clus = v.bits == 32 ? rd32(s + 44) : 0;
    v.hint = 2;
    /* power lost between a sector's erase and program (WL rewrites by erase
     * + write) leaves it all 0xFF; a FAT copy like that is rebuilt from the
     * other copy, which is written second, so one of them is always whole */
    for (uint32_t k = 0; v.nfats >= 2 && k < v.fatsz; k++) {
        uint8_t *a = cdat.buf, *m = cfat.buf;
        if (b->read(b, v.fat_start + k, a, 1) || b->read(b, v.fat_start + v.fatsz + k, m, 1)) return FAT_EIO;
        bool ea = true, em = true;
        for (uint32_t i = 0; i < v.bps && (ea || em); i++) { ea &= a[i] == 0xFF; em &= m[i] == 0xFF; }
        if (ea != em && b->write(b, ea ? v.fat_start + k : v.fat_start + v.fatsz + k, ea ? m : a, 1)) return FAT_EIO;
        if (ea != em) v.repaired++;
    }
    cdat.valid = cfat.valid = false;
    memset(files, 0, sizeof files);
    memset(dirs, 0, sizeof dirs);
    v.mounted = true;
    return 0;
}

int fat_unmount(void) { int r = fat_sync(); v.mounted = false; return r; }

uint32_t fat_repaired(void) { return v.repaired; }

void fat_info(uint32_t *total, uint32_t *freeb, int *bits)
{
    uint32_t f = 0;
    for (uint32_t c = 2; c < v.nclus + 2; c++) f += fat_get(c) == 0;
    if (total) *total = v.nclus * v.spc * v.bps;
    if (freeb) *freeb = f * v.spc * v.bps;
    if (bits) *bits = v.bits;
}

int fat_format(bdev_t *b)
{
    pcache_clear();
    uint32_t bps = b->sector_size, tot = b->sector_count, spc = 1, rsvd = 1, nf = 2, rootents = 512, fatsz = 1, nclus = 0;
    int bits = 12;
    if (bps > MAXSS || tot < 16) return FAT_EINVAL;
    for (;;) {                                                  /* smallest cluster that fits the type */
        for (int it = 0; it < 3; it++) {
            uint32_t rs = bits == 32 ? 0 : (rootents * 32 + bps - 1) / bps;
            nclus = (tot - rsvd - nf * fatsz - rs) / spc;
            fatsz = ((nclus + 2) * bits / 8 + bps - 1) / bps;
        }
        if (bits == 12 && nclus >= 4085) { bits = 16; continue; }
        if (bits == 16 && nclus >= 65525) { bits = 32; rsvd = 32; rootents = 0; spc = 8; continue; }
        break;
    }
    uint8_t *s = cdat.buf;
    cdat.valid = false; cfat.valid = false;
    memset(s, 0, bps);
    s[0] = 0xEB; s[1] = bits == 32 ? 0x58 : 0x3C; s[2] = 0x90;
    memcpy(s + 3, "MSDOS5.0", 8);
    wr16(s + 11, bps); s[13] = spc; wr16(s + 14, rsvd); s[16] = nf; wr16(s + 17, rootents);
    if (tot < 65536 && bits != 32) wr16(s + 19, tot); else wr32(s + 32, tot);
    s[21] = 0xF8; wr16(s + 24, 63); wr16(s + 26, 255);
    uint8_t *x = s + 36;
    if (bits == 32) { wr32(s + 36, fatsz); wr32(s + 44, 2); wr16(s + 48, 1); wr16(s + 50, 6); x = s + 64; }
    else wr16(s + 22, fatsz);
    x[0] = 0x80; x[2] = 0x29; wr32(x + 3, 0x1234ABCD);
    memcpy(x + 7, "NO NAME    ", 11);
    memcpy(x + 18, bits == 12 ? "FAT12   " : bits == 16 ? "FAT16   " : "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
    if (b->write(b, 0, s, 1)) return FAT_EIO;
    if (bits == 32 && b->write(b, 6, s, 1)) return FAT_EIO;    /* backup boot sector */
    memset(s, 0, bps);                                          /* clear FATs + root */
    uint32_t rs = bits == 32 ? 0 : (rootents * 32 + bps - 1) / bps;
    for (uint32_t i = 1; i < rsvd + nf * fatsz + rs; i++) if (i != 6 || bits != 32) if (b->write(b, i, s, 1)) return FAT_EIO;
    if (bits == 32) {                                           /* FSInfo + zeroed root cluster */
        wr32(s, 0x41615252); wr32(s + 484, 0x61417272); wr32(s + 488, 0xFFFFFFFF); wr32(s + 492, 3);
        s[510] = 0x55; s[511] = 0xAA;
        if (b->write(b, 1, s, 1)) return FAT_EIO;
        memset(s, 0, bps);
        for (uint32_t i = 0; i < spc; i++) if (b->write(b, rsvd + nf * fatsz + i, s, 1)) return FAT_EIO;
    }
    memset(s, 0, bps);                                          /* FAT[0], FAT[1] (+ root EOC) */
    if (bits == 12) { s[0] = 0xF8; s[1] = 0xFF; s[2] = 0xFF; }
    else if (bits == 16) { wr16(s, 0xFFF8); wr16(s + 2, 0xFFFF); }
    else { wr32(s, 0x0FFFFFF8); wr32(s + 4, 0x0FFFFFFF); wr32(s + 8, 0x0FFFFFFF); }
    for (uint32_t k = 0; k < nf; k++) if (b->write(b, rsvd + k * fatsz, s, 1)) return FAT_EIO;
    return fat_mount(b);
}
