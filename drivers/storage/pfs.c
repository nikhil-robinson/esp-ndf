/* PFS, power-safe log filesystem (see pfs.h for the guarantee).
 *
 * Partition = 4 KB blocks. Blocks [0, 2*MH) hold the metadata log as two halves of MH blocks; the rest is data.
 * Half = 16 B header {magic, seq, nblk, crc} + records {u32 crc, u8 type, u8 len, u16 id, payload}, 4 B aligned.
 * A half counts only after its COMMIT record (written at the end of a compaction); mount takes the committed
 * half with the highest seq and replays it up to the first record that fails its CRC.
 * Records: NEW(id, name), DEL(id), REN(id, name), EXT(id, file offset, len, flash address), SIZE(id, size).
 * EXT overrides that byte range of the file. File data is appended to one shared "head" block (always erased
 * space), one EXT per flush and block; nothing is overwritten in place. GC copies the live extents of the block
 * with the least live data to the head (new EXT records), which frees it; blocks are erased when reused.
 * RAM: the whole file table and extent list (12 B per extent), so reads and seeks never walk flash. */
#include "pfs.h"
#include "ndf_config.h"
#include "crc32.h"
#include "flash.h"
#include "rng.h"
#include <string.h>

#define BLK        4096
#define NAME       64
#define WBUF       256
#define MAGIC      0x31534650u          /* "PFS1" */
#define HDR        20
#define MAXREC     (8 + NAME)
#define MAX_NAMES  NDF_PFS_MAX_NAMES
#define MAX_EXT    NDF_PFS_MAX_EXTENTS
#define MAX_FD     NDF_PFS_MAX_FILES
#define MAX_DIR    NDF_PFS_MAX_DIRS
#define MAX_BLOCKS NDF_PFS_MAX_BLOCKS
#define NOEXT      0xFFFF
#define MORE       0x80                 /* record type flag: the flush group goes on */
#define GRP        16                   /* blocks per atomic group (64 KB) */

enum { R_NEW = 1, R_DEL, R_REN, R_EXT, R_SIZE, R_COMMIT };

static const partition_t *part;
static bool mounted, dead;
static int fault = -1;
static uint32_t nblk, mh, d0, seq, mpos, cursor, nonce, mer[2];   /* mer: blocks of each half erased so far */
static int act, hblk = -1;
static uint32_t hoff;
static struct { bool used; uint32_t size; char name[NAME]; } fl[MAX_NAMES];
static struct { uint32_t foff, addr; uint16_t len, fid, next; } ex[MAX_EXT];   /* an extent stays inside one block */
static uint16_t ehead[MAX_NAMES], efree;                  /* per-file extent lists + free list */
static int nfree;
static uint16_t live[MAX_BLOCKS];

typedef struct { bool used, trunc; uint16_t fid; int flags; uint32_t pos, woff, wn; uint8_t wbuf[WBUF]; } file_t;
static file_t fds[MAX_FD];
static struct { bool used; uint32_t next; char prefix[NAME]; size_t len; } dirs[MAX_DIR];

void pfs_fault_after(int n) { fault = n; dead = false; }

/* ---- flash with the fault hook ------------------------------------------ */

static int fw(uint32_t off, const void *b, size_t n)
{
    if (dead) return FAT_EIO;
    if (fault == 0) { partition_write(part, off, b, n / 2); dead = true; return FAT_EIO; }
    if (fault > 0) fault--;
    return partition_write(part, off, b, n) ? FAT_EIO : 0;
}

static int fe(uint32_t off, size_t n)
{
    if (dead) return FAT_EIO;
    if (fault == 0) { dead = true; return FAT_EIO; }
    if (fault > 0) fault--;
    return partition_erase(part, off, n) ? FAT_EIO : 0;
}

static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint32_t hbase(int h) { return h * mh * BLK; }

/* ---- RAM model: extents ----------------------------------------------- */

static int ext_new(uint16_t fid, uint32_t foff, uint32_t len, uint32_t addr, uint16_t next)
{
    if (efree == NOEXT) return -1;
    int i = efree;
    efree = ex[i].next; nfree--;
    ex[i].fid = fid; ex[i].foff = foff; ex[i].len = len; ex[i].addr = addr; ex[i].next = next;
    return i;
}
static void ext_put(int i) { ex[i].fid = NOEXT; ex[i].next = efree; efree = i; nfree++; }
static int ext_free(void) { return nfree; }

/* drop [a, b) of the file; with addr != NOADDR also add the new extent there (merged into its neighbour
 * when contiguous in the file and on flash). One walk of the file's own list. */
#define NOADDR 0xFFFFFFFFu
static int ext_cut_add(uint16_t fid, uint32_t a, uint32_t b, uint32_t addr)
{
    int mg = -1, h = ehead[fid];
    if (addr != NOADDR && a >= fl[fid].size) {            /* append (the log case): nothing to cut, and the */
        fl[fid].size = b;                                 /* only merge candidate is the newest extent, the head */
        if (h != NOEXT && ex[h].foff + ex[h].len == a && ex[h].addr + ex[h].len == addr &&
            ex[h].addr / BLK == (addr + (b - a) - 1) / BLK) { ex[h].len += b - a; return 0; }
        int j = ext_new(fid, a, b - a, addr, h);
        if (j < 0) return FAT_ENOSPC;
        ehead[fid] = j;
        return 0;
    }
    for (uint16_t *pp = &ehead[fid]; *pp != NOEXT; ) {
        int i = *pp;
        uint32_t s = ex[i].foff, e = s + ex[i].len;
        if (s >= b || e < a) { pp = &ex[i].next; continue; }
        if (e == a || (s < a && e <= b)) {                /* keeps [s, a): ends where the new data starts */
            ex[i].len = a - s;
            if (addr != NOADDR && ex[i].addr + ex[i].len == addr && ex[i].addr / BLK == (addr + (b - a) - 1) / BLK) mg = i;
            pp = &ex[i].next;
        } else if (s >= a && e <= b) { *pp = ex[i].next; ext_put(i); }
        else if (s < a) {                                 /* [a, b) strictly inside: split */
            int j = ext_new(fid, b, e - b, ex[i].addr + (b - s), ex[i].next);
            if (j < 0) return FAT_ENOSPC;
            ex[i].len = a - s; ex[i].next = j;
            pp = &ex[j].next;
        } else { ex[i].addr += b - s; ex[i].len = e - b; ex[i].foff = b; pp = &ex[i].next; }
    }
    if (addr == NOADDR) return 0;
    if (b > fl[fid].size) fl[fid].size = b;
    if (mg >= 0) { ex[mg].len += b - a; return 0; }
    int j = ext_new(fid, a, b - a, addr, ehead[fid]);
    if (j < 0) return FAT_ENOSPC;
    ehead[fid] = j;
    return 0;
}

static void ext_drop(uint16_t fid)
{
    while (ehead[fid] != NOEXT) { int i = ehead[fid]; ehead[fid] = ex[i].next; ext_put(i); }
}

static int apply(const uint8_t *r)
{
    uint16_t id = r[6] | r[7] << 8;
    int type = r[4] & ~MORE;
    if (type != R_COMMIT && id >= MAX_NAMES) return FAT_EINVAL;
    switch (type) {
    case R_NEW: fl[id].used = true; fl[id].size = 0; ext_drop(id); /* fallthrough */
    case R_REN: memcpy(fl[id].name, r + 8, NAME); fl[id].name[NAME - 1] = 0; return 0;
    case R_DEL: fl[id].used = false; ext_drop(id); return 0;
    case R_EXT: return ext_cut_add(id, rd32(r + 8), rd32(r + 8) + rd32(r + 12), rd32(r + 16));
    case R_SIZE: fl[id].size = rd32(r + 8); return ext_cut_add(id, fl[id].size, 0xFFFFFFFF, NOADDR);
    case R_COMMIT: return 0;
    }
    return FAT_EINVAL;
}

/* ---- metadata log ------------------------------------------------------- */

/* record CRCs are seeded with the half's seq and the format's random nonce: records left from an older use of a
 * block (log blocks are erased only when the log reaches them) can never pass as current */
static uint32_t seed(uint32_t sq) { return nonce ^ sq * 0x9E3779B1u; }

static int rec_build(uint8_t *r, uint32_t sd, int type, uint16_t id, const void *pl, size_t n)
{
    size_t len = (8 + n + 3) & ~3u;
    memset(r, 0, len);
    r[4] = type; r[5] = len; r[6] = id; r[7] = id >> 8;
    if (n) memcpy(r + 8, pl, n);
    wr32(r, crc32_le(sd, r + 4, len - 4));
    return len;
}

static int log_write(int h, uint32_t pos, const void *b, uint32_t n)   /* erases the log blocks it reaches */
{
    while ((pos + n - 1) / BLK >= mer[h]) {
        int e = fe(hbase(h) + mer[h] * BLK, BLK);
        if (e) return e;
        mer[h]++;
    }
    return fw(hbase(h) + pos, b, n);
}

static int rec_raw(int h, uint32_t *pos, int type, uint16_t id, const void *pl, size_t n)
{
    uint8_t r[MAXREC + 4];
    int len = rec_build(r, seed(seq + 1), type, id, pl, n);
    if (*pos + len > mh * BLK) return FAT_ENOSPC;
    int e = log_write(h, *pos, r, len);
    if (!e) *pos += len;
    return e;
}

static int name_rec(int h, uint32_t *pos, int type, uint16_t id)
{
    return rec_raw(h, pos, type, id, fl[id].name, strlen(fl[id].name) + 1);
}

/* snapshot of the RAM state into the other half; it becomes the active half once COMMIT is on flash */
static int compact(void)
{
    int t = act ^ 1, e;
    uint32_t pos = HDR;
    uint8_t h[HDR];
    wr32(h, MAGIC); wr32(h + 4, seq + 1); wr32(h + 8, nblk); wr32(h + 12, nonce); wr32(h + 16, crc32_le(0, h, 16));
    mer[t] = 0;
    if ((e = log_write(t, 0, h, HDR))) return e;
    for (int id = 0; id < MAX_NAMES; id++) {
        if (!fl[id].used) continue;
        if ((e = name_rec(t, &pos, R_NEW, id))) return e;
        for (int i = ehead[id]; i != NOEXT; i = ex[i].next) {
            uint8_t p[12]; wr32(p, ex[i].foff); wr32(p + 4, ex[i].len); wr32(p + 8, ex[i].addr);
            if ((e = rec_raw(t, &pos, R_EXT, id, p, 12))) return e;
        }
        uint8_t p[4]; wr32(p, fl[id].size);
        if ((e = rec_raw(t, &pos, R_SIZE, id, p, 4))) return e;
    }
    if ((e = rec_raw(t, &pos, R_COMMIT, 0, NULL, 0))) return e;
    act = t; seq++; mpos = pos;
    return 0;
}

/* append one record to the active half (compacting first if it is full), then apply it to RAM */
static int rec_put(int type, uint16_t id, const void *pl, size_t n)
{
    uint8_t r[MAXREC + 4];
    int len = (8 + n + 3) & ~3u, e;
    if (mpos + len > mh * BLK && (e = compact())) return e;
    if (mpos + len > mh * BLK) return FAT_ENOSPC;
    rec_build(r, seed(seq), type, id, pl, n);             /* after compact(): the seq may have moved on */
    if ((e = log_write(act, mpos, r, len))) return e;
    mpos += len;
    return apply(r);
}

/* ---- data blocks -------------------------------------------------------- */

static uint32_t count_free(void)
{
    memset(live, 0, sizeof live);
    for (int i = 0; i < MAX_EXT; i++) if (ex[i].fid != NOEXT) live[ex[i].addr / BLK] += ex[i].len;
    uint32_t n = 0;
    for (uint32_t b = d0; b < nblk; b++) n += !live[b] && (int)b != hblk;
    return n;
}

static int pick_free(void)                                /* rotating start: spreads the erases; live[] fresh */
{
    for (uint32_t k = 0; k < nblk - d0; k++) {
        uint32_t b = d0 + (cursor + k) % (nblk - d0);
        if (live[b] || (int)b == hblk) continue;
        live[b] = 1;                                      /* taken: a second pick skips it */
        cursor = b + 1 - d0;
        return b;
    }
    return -1;
}

/* next head block: from the group's reservation, else any free block (GC moves) */
static uint16_t resv[GRP];
static int nresv, iresv;
static int head_next(void)
{
    if (hblk >= 0 && hoff < BLK) return 0;
    int b;
    if (iresv < nresv) b = resv[iresv++];
    else { count_free(); if ((b = pick_free()) < 0) return FAT_ENOSPC; }
    int e = fe(b * BLK, BLK);
    if (!e) { hblk = b; hoff = 0; }
    return e;
}

/* program c bytes at the head, from RAM or (src == NULL) copied from flash at src_addr */
static int program(const uint8_t *src, uint32_t src_addr, uint32_t c)
{
    uint32_t at = hblk * BLK + hoff;
    int e = 0;
    if (src) e = fw(at, src, c);
    else for (uint32_t d = 0; d < c && !e; d += WBUF) {
        uint8_t t[WBUF]; uint32_t k = c - d < WBUF ? c - d : WBUF;
        e = partition_read(part, src_addr + d, t, k) ? FAT_EIO : fw(at + d, t, k);
    }
    if (!e) hoff += c;
    return e;
}

static int ext_rec(int type, uint16_t fid, uint32_t foff, uint32_t c, uint32_t at)
{
    uint8_t p[12]; wr32(p, foff); wr32(p + 4, c); wr32(p + 8, at);
    return rec_put(type, fid, p, 12);
}

/* GC: move the live extents of the block with the least live data to the head; that block becomes free.
 * Each move stands alone (old and new copy hold the same bytes), so a cut mid-GC loses nothing. */
static int gc_one(void)
{
    count_free();
    uint32_t v = 0, best = BLK;
    for (uint32_t b = d0; b < nblk; b++)
        if (live[b] && live[b] < best && (int)b != hblk) { best = live[b]; v = b; }
    if (!v) return FAT_ENOSPC;                            /* every block is full of live data */
    for (;;) {
        int i = 0;
        while (i < MAX_EXT && (ex[i].fid == NOEXT || ex[i].addr / BLK != v)) i++;
        if (i == MAX_EXT) return 0;
        uint16_t fid = ex[i].fid;
        uint32_t foff = ex[i].foff, src = ex[i].addr, len = ex[i].len;
        while (len) {
            int e = head_next();
            if (e) return e;
            if (ext_free() < 2 && (e = FAT_ENOSPC)) return e;
            uint32_t c = BLK - hoff < len ? BLK - hoff : len, at = hblk * BLK + hoff;
            if ((e = program(NULL, src, c)) || (e = ext_rec(R_EXT, fid, foff, c, at))) return e;
            foff += c; src += c; len -= c;
        }
    }
}

/* one atomic flush: [lazy truncate] + len bytes at foff, all records but the last flagged MORE. Everything the
 * group needs is secured first (blocks reserved after GC, log room, extent slots), so nothing runs in between
 * and no block it supersedes can be erased before its last record is on flash. */
static int put_group(uint16_t fid, uint32_t foff, const uint8_t *src, uint32_t len, bool trunc)
{
    uint32_t k, recs, nf = count_free();
    for (uint32_t pass = 0;; pass++) {
        uint32_t c0 = hblk >= 0 ? BLK - hoff : 0, rest = len > c0 ? len - c0 : 0;
        k = (rest + BLK - 1) / BLK;
        recs = (c0 && len) + k + trunc;
        if (nf >= k + 1) break;                           /* one block stays reserved for GC */
        int e = pass < nblk ? gc_one() : FAT_ENOSPC;      /* bounded: a nearly full volume gains little per pass */
        if (e) return e;
        nf = count_free();
    }
    for (nresv = iresv = 0; nresv < (int)k; nresv++) resv[nresv] = pick_free();
    int e = 0;
    if (mpos + recs * 20 > mh * BLK) e = compact();
    if (!e && (mpos + recs * 20 > mh * BLK || ext_free() < (int)(2 * recs))) e = FAT_ENOSPC;
    if (!e && trunc) { uint8_t z[4] = { 0 }; e = rec_put(R_SIZE | (len ? MORE : 0), fid, z, 4); }
    while (!e && len) {
        if ((e = head_next())) break;
        uint32_t c = BLK - hoff < len ? BLK - hoff : len, at = hblk * BLK + hoff;
        if ((e = program(src, 0, c))) break;
        e = ext_rec(R_EXT | (c < len ? MORE : 0), fid, foff, c, at);
        foff += c; src += c; len -= c;
    }
    nresv = iresv = 0;
    return e;
}

/* user data: atomic per group of up to (GRP - 1) blocks */
static int put_data(uint16_t fid, uint32_t foff, const uint8_t *src, uint32_t len, bool trunc)
{
    do {
        uint32_t g = len < (GRP - 1) * BLK ? len : (GRP - 1) * BLK;
        int e = put_group(fid, foff, src, g, trunc);
        if (e) return e;
        trunc = false; foff += g; src += g; len -= g;
    } while (len);
    return 0;
}

/* ---- mount / format ----------------------------------------------------- */

static int geometry(const partition_t *p)
{
    nblk = p->size / BLK;
    mh = nblk / 32 < 2 ? 2 : nblk / 32;
    d0 = 2 * mh;
    if (nblk > MAX_BLOCKS || nblk < d0 + 4) return FAT_EINVAL;
    part = p;
    return 0;
}

static void reset_ram(void)
{
    memset(fl, 0, sizeof fl);
    for (int i = 0; i < MAX_EXT; i++) { ex[i].fid = NOEXT; ex[i].next = i + 1 < MAX_EXT ? i + 1 : NOEXT; }
    for (int i = 0; i < MAX_NAMES; i++) ehead[i] = NOEXT;
    efree = 0; nfree = MAX_EXT;
    memset(fds, 0, sizeof fds); memset(dirs, 0, sizeof dirs);
    hblk = -1; hoff = 0;
}

int pfs_format(const partition_t *p)
{
    int e = geometry(p);
    if (e) return e;
    dead = false; mounted = false; reset_ram();
    if ((e = fe(hbase(1), BLK))) return e;                /* no old header left in half 1; the rest is erased as reached */
    mer[1] = 1;
    nonce = rng_u32();
    seq = 0; act = 1;                                     /* compact() writes half 0, seq 1 */
    return compact();
}

static bool blank(const uint8_t *b, size_t n) { while (n--) if (*b++ != 0xFF) return false; return true; }

/* replay half h (seq and nonce set); true when it holds a COMMIT. Reads 512 B windows: one flash read per ~30
 * records. Sets mer[h]: a stop at a block start means that block was not erased yet (or holds a cut record):
 * it is erased before the next write. A bad record inside a block is torn: mount compacts. */
static bool replay(int h, bool *torn)
{
    static uint8_t w[512], pend[GRP + 2][20];            /* an unfinished group at the end is dropped */
    int np = 0;
    uint32_t wb = 0, wl = 0, pos = HDR, end = mh * BLK;
    bool committed = false;
    *torn = false;
    reset_ram();
    while (pos + 8 <= end) {
        if (pos < wb || pos + MAXREC > wb + wl) {
            wb = pos; wl = end - pos < sizeof w ? end - pos : sizeof w;
            if (partition_read(part, hbase(h) + wb, w, wl)) return false;
        }
        const uint8_t *r = w + pos - wb;
        uint32_t len = r[5], room = wl - (pos - wb);
        uint32_t chk = room < MAXREC ? room : MAXREC;     /* blank up to the block end: the next block may be stale */
        if (chk > BLK - pos % BLK) chk = BLK - pos % BLK;
        if (blank(r, chk)) break;
        if (len < 8 || len > MAXREC || len & 3 || pos + len > end || rd32(r) != crc32_le(seed(seq), r + 4, len - 4)) {
            *torn = pos % BLK != 0;
            break;
        }
        if (np && r[6] != pend[0][6]) { *torn = true; break; }   /* a group is one file's, back to back */
        if (r[4] & MORE) {                                /* held until the group's last record */
            if (np == GRP + 2 || len > 20) { *torn = true; break; }
            memcpy(pend[np++], r, len);
        } else {
            bool bad = false;
            for (int i = 0; i < np; i++) bad |= apply(pend[i]) != 0;
            np = 0;
            if (bad || apply(r)) { *torn = true; break; }
        }
        committed |= r[4] == R_COMMIT;
        pos += len;
    }
    if (np) *torn = true;                                 /* unfinished group: never append behind it */
    mpos = pos;
    mer[h] = pos >= end ? mh : pos / BLK + (pos % BLK != 0);
    return committed;
}

int pfs_mount(const partition_t *p)
{
    int e = geometry(p);
    if (e) return e;
    dead = false; mounted = false;
    uint32_t s[2] = { 0, 0 }, nc[2];
    for (int h = 0; h < 2; h++) {
        uint8_t b[HDR];
        if (partition_read(part, hbase(h), b, HDR)) return FAT_EIO;
        if (rd32(b) == MAGIC && rd32(b + 8) == nblk && rd32(b + 16) == crc32_le(0, b, 16)) s[h] = rd32(b + 4), nc[h] = rd32(b + 12);
    }
    int order[2] = { s[1] > s[0], s[1] <= s[0] };
    for (int k = 0; k < 2; k++) {
        int h = order[k];
        bool torn;
        if (!s[h]) continue;
        seq = s[h]; nonce = nc[h];
        if (!replay(h, &torn)) continue;
        act = h; cursor = 0;
        if (torn && (e = compact())) return e;           /* never append after torn bytes */
        mounted = true;
        return 0;
    }
    reset_ram();
    return FAT_EINVAL;
}

/* ---- files -------------------------------------------------------------- */

static file_t *get(int fd) { return fd >= 0 && fd < MAX_FD && fds[fd].used ? &fds[fd] : NULL; }
static int find(const char *path) { for (int i = 0; i < MAX_NAMES; i++) if (fl[i].used && !strcmp(fl[i].name, path)) return i; return -1; }
static bool is_open(int id) { for (int f = 0; f < MAX_FD; f++) if (fds[f].used && fds[f].fid == id) return true; return false; }
static uint32_t fsize(file_t *f) { uint32_t s = f->trunc ? 0 : fl[f->fid].size; return f->wn && f->woff + f->wn > s ? f->woff + f->wn : s; }

static int flush(file_t *f)                               /* the pending truncate goes in the same group */
{
    if (!f->wn && !f->trunc) return 0;
    int e = put_data(f->fid, f->woff, f->wbuf, f->wn, f->trunc);
    f->wn = 0; f->trunc = false;
    return e;
}

int pfs_unmount(void)
{
    int r = 0;
    for (int f = 0; f < MAX_FD; f++) if (fds[f].used) { int e = flush(&fds[f]); if (e) r = e; fds[f].used = false; }
    mounted = false;
    return r;
}

int pfs_open(const char *path, int flags)
{
    if (!mounted) return FAT_EINVAL;
    size_t len = strlen(path);
    if (!len || len >= NAME) return FAT_EINVAL;
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return FAT_EMFILE;
    int id = find(path), e;
    if (id < 0) {
        if (!(flags & FAT_CREAT)) return FAT_ENOENT;
        for (id = 0; id < MAX_NAMES && fl[id].used; id++) ;
        if (id == MAX_NAMES) return FAT_ENOSPC;
        if ((e = rec_put(R_NEW, id, path, len + 1))) return e;
    }
    fds[fd] = (file_t){ .used = true, .fid = id, .flags = flags, .trunc = (flags & FAT_TRUNC) && (flags & 3) && fl[id].size };
    fds[fd].pos = flags & FAT_APPEND ? fsize(&fds[fd]) : 0;
    return fd;
}

int pfs_read(int fd, void *buf, size_t n)
{
    file_t *f = get(fd);
    if (!f || (f->flags & 3) == FAT_WRONLY) return FAT_EINVAL;
    int e = flush(f);                                     /* own writes become visible to the read */
    if (e) return e;
    uint32_t size = fl[f->fid].size;
    if (f->pos >= size) return 0;
    if (n > size - f->pos) n = size - f->pos;
    uint8_t *d = buf;
    for (size_t left = n; left; ) {
        uint32_t c = 0, next = size;
        for (int i = ehead[f->fid]; i != NOEXT; i = ex[i].next) {
            uint32_t s = ex[i].foff, en = s + ex[i].len;
            if (s <= f->pos && f->pos < en) {
                c = en - f->pos < left ? en - f->pos : left;
                if (partition_read(part, ex[i].addr + f->pos - s, d, c)) return FAT_EIO;
                break;
            }
            if (s > f->pos && s < next) next = s;
        }
        if (!c) { c = next - f->pos < left ? next - f->pos : left; memset(d, 0, c); }   /* hole */
        d += c; f->pos += c; left -= c;
    }
    return n;
}

int pfs_write(int fd, const void *buf, size_t n)
{
    file_t *f = get(fd);
    if (!f || (f->flags & 3) == FAT_RDONLY) return FAT_EINVAL;
    if (f->flags & FAT_APPEND) f->pos = fsize(f);
    const uint8_t *s = buf;
    size_t left = n;
    while (left) {
        int e = 0;
        if (f->wn && f->pos == f->woff + f->wn && f->wn < WBUF) {
            uint32_t c = WBUF - f->wn < left ? WBUF - f->wn : left;
            memcpy(f->wbuf + f->wn, s, c); f->wn += c; s += c; left -= c; f->pos += c;
            if (f->wn == WBUF) e = flush(f);
        } else if (f->wn) e = flush(f);
        else if (left >= WBUF) { e = put_data(f->fid, f->pos, s, left, f->trunc); if (!e) { f->trunc = false; f->pos += left; left = 0; } }
        else { f->woff = f->pos; memcpy(f->wbuf, s, left); f->wn = left; f->pos += left; left = 0; }
        if (e) return n - left ? (int)(n - left) : e;
    }
    return n;
}

int pfs_seek(int fd, int32_t off, int whence)
{
    file_t *f = get(fd);
    if (!f) return FAT_EINVAL;
    int32_t base = whence == FAT_SEEK_SET ? 0 : whence == FAT_SEEK_CUR ? (int32_t)f->pos : (int32_t)fsize(f);
    if (base + off < 0 || (uint32_t)(base + off) > fsize(f)) return FAT_EINVAL;
    f->pos = base + off;
    return f->pos;
}

int pfs_sync(int fd) { file_t *f = get(fd); return f ? flush(f) : FAT_EINVAL; }

int pfs_close(int fd)
{
    file_t *f = get(fd);
    if (!f) return FAT_EINVAL;
    int r = flush(f);
    f->used = false;
    return r;
}

int pfs_unlink(const char *path)
{
    int id = mounted ? find(path) : -1;
    if (id < 0) return FAT_ENOENT;
    if (is_open(id)) return FAT_EINVAL;
    return rec_put(R_DEL, id, NULL, 0);
}

int pfs_rename(const char *from, const char *to)
{
    size_t len = strlen(to);
    if (!mounted || !len || len >= NAME) return FAT_EINVAL;
    int id = find(from);
    if (id < 0) return FAT_ENOENT;
    if (find(to) >= 0) return FAT_EEXIST;
    if (is_open(id)) return FAT_EINVAL;
    return rec_put(R_REN, id, to, len + 1);
}

int pfs_stat(const char *path, uint32_t *size, bool *is_dir)
{
    bool root = !strcmp(path, "/");
    if (is_dir) *is_dir = root;
    if (root) { if (size) *size = 0; return 0; }
    int id = mounted ? find(path) : -1;
    if (id < 0) return FAT_ENOENT;
    if (size) *size = fl[id].size;
    return 0;
}

int pfs_opendir(const char *path)
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

int pfs_readdir(int dd, char *name, uint32_t *size, bool *is_dir)
{
    if (dd < 0 || dd >= MAX_DIR || !dirs[dd].used) return FAT_EINVAL;
    while (dirs[dd].next < MAX_NAMES) {
        int i = dirs[dd].next++;
        const char *n = fl[i].name;
        size_t pl = dirs[dd].len;
        if (!fl[i].used || strncmp(n, dirs[dd].prefix, pl) || n[pl] != '/') continue;
        strcpy(name, n + pl + 1);
        if (size) *size = fl[i].size;
        if (is_dir) *is_dir = false;
        return 1;
    }
    return 0;
}

int pfs_closedir(int dd)
{
    if (dd < 0 || dd >= MAX_DIR) return FAT_EINVAL;
    dirs[dd].used = false;
    return 0;
}

void pfs_info(uint32_t *total, uint32_t *used)
{
    uint32_t u = 0;
    for (int i = 0; i < MAX_EXT; i++) if (ex[i].fid != NOEXT) u += ex[i].len;
    if (total) *total = (nblk - d0) * BLK;
    if (used) *used = u;
}
