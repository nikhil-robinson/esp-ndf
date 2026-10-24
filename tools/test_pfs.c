/* Host power-cut fuzz test for drivers/storage/pfs.c on a simulated NOR flash (program = AND, erase = 0xFF).
 * Random transactions (replace a file, append to a log, patch the middle, rename, unlink) with a cut injected
 * after a random number of flash operations; after each cut the volume is remounted and must hold exactly the
 * state before or after the cut transaction (a file created by it may also exist empty).
 * STRICT=1 also remounts after every transaction (finds the first one whose log replays differently).
 *   gcc -O2 -Iinclude -Isoc/esp32s3 tools/test_pfs.c drivers/storage/pfs.c lib/crc32.c -o /tmp/t && /tmp/t [cuts] */
#include "pfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FSZ (1024 * 1024)
static uint8_t nor[FSZ];
static const partition_t P = { .type = PART_DATA, .subtype = PART_SUB_SPIFFS, .offset = 0, .size = FSZ, .label = "t" };
int partition_read(const partition_t *p, uint32_t o, void *b, size_t n) { (void)p; if (o + n > FSZ) return -1; memcpy(b, nor + o, n); return 0; }
int partition_write(const partition_t *p, uint32_t o, const void *b, size_t n) { (void)p; if (o + n > FSZ) return -1; for (size_t i = 0; i < n; i++) nor[o + i] &= ((const uint8_t *)b)[i]; return 0; }
int partition_erase(const partition_t *p, uint32_t o, size_t n) { (void)p; if (o % 4096 || n % 4096 || o + n > FSZ) return -1; memset(nor + o, 0xFF, n); return 0; }

#define NF 8
#define MAXF 40000
#define BAL (820 * 1024)
typedef struct { bool ex[NF]; uint32_t len[NF]; uint8_t d[NF][MAXF]; } model_t;
static model_t cur, nxt;
static uint8_t buf[MAXF + 1];
static const char *nm(int i) { static char s[NF][16]; sprintf(s[i], "/f%d.bin", i); return s[i]; }
#define FAIL(...) do { printf(__VA_ARGS__); printf("\n"); exit(1); } while (0)

/* volume == model m? (created: file c may exist empty instead of absent) */
static bool verbose; static int lastop;
static bool same(const model_t *m, int created)
{
    if (verbose) for (int i = 0; i < NF; i++) {
        uint32_t sz = 0; int r = pfs_stat(nm(i), &sz, NULL);
        int fd = pfs_open(nm(i), FAT_RDONLY), n = fd >= 0 ? pfs_read(fd, buf, MAXF + 1) : -1; pfs_close(fd);
        uint32_t k = 0; while (r == 0 && m->ex[i] && k < sz && k < m->len[i] && buf[k] == m->d[i][k]) k++;
        printf("  %s: vol %s %u (read %d) model %s %u, first diff %u\n", nm(i), r ? "-" : "y", sz, n, m->ex[i] ? "y" : "-", m->len[i], k);
    }
    for (int i = 0; i < NF; i++) {
        uint32_t sz; bool d;
        int r = pfs_stat(nm(i), &sz, &d);
        if (!m->ex[i]) { if (r == 0 && !(i == created && sz == 0)) return false; if (r) continue; }
        if (m->ex[i] && r) return false;
        if (!m->ex[i]) continue;
        if (sz != m->len[i]) return false;
        int fd = pfs_open(nm(i), FAT_RDONLY);
        int n = pfs_read(fd, buf, MAXF + 1);
        pfs_close(fd);
        if (n != (int)sz || memcmp(buf, m->d[i], sz)) return false;
    }
    return true;
}

static int err, trunc_i;                                           /* last error: FAT_ENOSPC (volume full) or FAT_EIO (cut) */
static int w_all(int fd, const uint8_t *s, uint32_t n)
{
    int r = pfs_write(fd, s, n);
    return r == (int)n ? 0 : (err = r < 0 ? r : FAT_ENOSPC);
}
#define CHK(x) do { int r_ = (x); if (r_ < 0) return err = r_; } while (0)

/* one random transaction on the volume and on nxt (= cur + its effect); -1 when the flash "died" */
static int txn(int *created)
{
    nxt = cur; *created = -1; trunc_i = -1;
    int i = rand() % NF, op = rand() % 10, fd; lastop = op * 100 + i;
    if (op < 4) {                                         /* replace: open TRUNC, write, close */
        uint32_t n = rand() % 4 ? rand() % 300 : rand() % MAXF;
        for (uint32_t k = 0; k < n; k++) nxt.d[i][k] = rand();
        if (!cur.ex[i]) *created = i;
        nxt.ex[i] = true; nxt.len[i] = n; trunc_i = i;
        CHK(fd = pfs_open(nm(i), FAT_WRONLY | FAT_CREAT | FAT_TRUNC));
        CHK(w_all(fd, nxt.d[i], n));
        CHK(pfs_close(fd));
        return 0;
    }
    if (op < 7) {                                         /* append a log line (< 256 B: one buffer) */
        uint32_t n = 1 + rand() % 200, o = cur.ex[i] ? cur.len[i] : 0;
        if (o + n > MAXF) { nxt.ex[i] = false; CHK(pfs_unlink(nm(i))); return 0; }   /* full log: start over */
        for (uint32_t k = 0; k < n; k++) nxt.d[i][o + k] = rand();
        if (!cur.ex[i]) *created = i;
        nxt.ex[i] = true; nxt.len[i] = o + n;
        CHK(fd = pfs_open(nm(i), FAT_WRONLY | FAT_CREAT | FAT_APPEND));
        uint32_t a = rand() % (n + 1);                    /* in two writes: one buffer, one flush */
        CHK(w_all(fd, nxt.d[i] + o, a)); CHK(w_all(fd, nxt.d[i] + o + a, n - a));
        CHK(pfs_sync(fd)); CHK(pfs_close(fd));
        return 0;
    }
    if (op < 8 && cur.ex[i] && cur.len[i]) {              /* patch inside */
        uint32_t o = rand() % cur.len[i], n = 1 + rand() % (cur.len[i] - o);
        for (uint32_t k = 0; k < n; k++) nxt.d[i][o + k] = rand();
        CHK(fd = pfs_open(nm(i), FAT_RDWR));
        CHK(pfs_seek(fd, o, FAT_SEEK_SET));
        CHK(w_all(fd, nxt.d[i] + o, n));
        CHK(pfs_close(fd));
        return 0;
    }
    if (op < 9) {                                         /* rename i -> j */
        int j = rand() % NF;
        if (!cur.ex[i] || cur.ex[j]) return 0;
        nxt.ex[j] = true; nxt.len[j] = cur.len[i]; memcpy(nxt.d[j], cur.d[i], cur.len[i]); nxt.ex[i] = false;
        CHK(pfs_rename(nm(i), nm(j)));
        return 0;
    }
    if (!cur.ex[i]) return 0;
    nxt.ex[i] = false;
    CHK(pfs_unlink(nm(i)));
    return 0;
}

int main(int argc, char **argv)
{
    int cuts = argc > 1 ? atoi(argv[1]) : 2000, done = 0, old = 0, new = 0, full = 0;
    long txns = 0;
    srand(argc > 2 ? atoi(argv[2]) : 1);
    memset(nor, 0x5A, sizeof nor);                        /* not erased: format must cope */
    if (pfs_format(&P) || pfs_mount(&P)) FAIL("format/mount");
    if (pfs_mount(&P)) FAIL("remount");
    static uint8_t ballast[BAL];                          /* fills the volume to ~85%: GC under pressure */
    for (uint32_t k = 0; k < BAL; k++) ballast[k] = k * 13 + (k >> 11);
    int bfd = pfs_open("/ballast", FAT_WRONLY | FAT_CREAT);
    if (pfs_write(bfd, ballast, BAL) != BAL || pfs_close(bfd)) FAIL("ballast");
    while (done < cuts) {
        pfs_fault_after(rand() % 400);
        int created, r;
        while ((r = txn(&created)) == 0 || err == FAT_ENOSPC) {
            if (r) {                                      /* full: nothing may have changed, fds stay usable */
                full++; err = 0;
                for (int f = 0; f < 4; f++) pfs_close(f);
                uint32_t tsz;                             /* O_TRUNC done at close, unless that failed too */
                if (trunc_i >= 0 && pfs_stat(nm(trunc_i), &tsz, NULL) == 0 && tsz == 0) { cur.ex[trunc_i] = true; cur.len[trunc_i] = 0; }
                if (!same(&cur, created)) { verbose = true; same(&cur, created); FAIL("ENOSPC changed the volume (txn %ld, op %d)", txns, lastop); }
                if (created >= 0 && pfs_stat(nm(created), NULL, NULL) == 0) { cur.ex[created] = true; cur.len[created] = 0; }
                continue;
            }
            cur = nxt; txns++; if (getenv("STRICT") && (pfs_mount(&P) || !same(&cur, -1))) { verbose = true; same(&cur, -1); FAIL("live mismatch txn %ld op %d", txns, lastop); } }
        pfs_fault_after(-1); err = 0;
        if (pfs_mount(&P)) FAIL("mount after cut %d", done);
        if (same(&nxt, created)) { cur = nxt; new++; }
        else if (same(&cur, created)) old++;
        else { verbose = true; printf("new:\n"); same(&nxt, created); printf("old:\n"); same(&cur, created); FAIL("cut %d (txn %ld): volume matches neither the old nor the new state", done, txns); }
        /* an existing-but-empty created file is a real file now */
        if (created >= 0 && !cur.ex[created] && pfs_stat(nm(created), NULL, NULL) == 0) { cur.ex[created] = true; cur.len[created] = 0; }
        done++;
        if (!same(&cur, -1)) FAIL("model sync %d", done);
        if (done == cuts / 2) {                           /* re-format over a used volume: stale logs everywhere */
            if (pfs_format(&P) || pfs_mount(&P)) FAIL("re-format");
            memset(&cur, 0, sizeof cur);
            bfd = pfs_open("/ballast", FAT_WRONLY | FAT_CREAT);
            if (pfs_write(bfd, ballast, BAL) != BAL || pfs_close(bfd) || pfs_mount(&P) || !same(&cur, -1)) FAIL("after re-format");
        }
        if (done % 500 == 0 || done == cuts) {
            static uint8_t b2[BAL + 1];
            int fd = pfs_open("/ballast", FAT_RDONLY), n = pfs_read(fd, b2, BAL + 1); pfs_close(fd);
            if (n != BAL || memcmp(b2, ballast, BAL)) FAIL("ballast damaged at cut %d", done);
        }
    }
    uint32_t t, u; pfs_info(&t, &u);
    printf("pfs power-cut fuzz: %d cuts over %ld transactions (%d refused full): %d rolled back, %d completed, 0 torn (%u KB used of %u)\n",
           cuts, txns, full, old, new, u / 1024, t / 1024);
    return 0;
}
uint32_t rng_u32(void) { return (uint32_t)rand() * 2654435761u ^ (uint32_t)rand(); }
