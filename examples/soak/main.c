/* Long-run soak: one round = SHA-256 of 64 KB (hardware, compared with round 0), AES-256-GCM round trip of 16 KB,
 * PSRAM 4 MB pattern (new seed each round), FAT write+read of a 32 KB file with CRC, events + cooperative tasks,
 * temperature. TG0 watchdog fed per round, restart on panic; crash records and restarts are reported at boot.
 * Host: tools/soak.py logs every line with a timestamp (bench/soak/data). */
#include "console.h"
#include "sys.h"
#include "system.h"
#include "sha.h"
#include "aes.h"
#include "rng.h"
#include "crc32.h"
#include "psram.h"
#include "fat.h"
#include "wl.h"
#include "partition.h"
#include "event.h"
#include "coop.h"
#include "tsens.h"
#include "wdt.h"
#include "heap.h"
#include <string.h>

static uint8_t buf[65536] __attribute__((aligned(4))), out[16384] __attribute__((aligned(4))), dec[16384] __attribute__((aligned(4)));
static RTC_NOINIT_ATTR uint32_t rounds_total, boots, magic;
static volatile int ev_n, co_n;
static void on_ev(uint16_t id, uint32_t d, void *a) { (void)id; (void)a; ev_n += d; }
static void task(void *a) { for (int i = 0; i < 50; i++) { co_n++; coop_yield(); } (void)a; }
static uint8_t st[2][2048] __attribute__((aligned(16)));

int main(void)
{
    if (magic != 0x534F414Bu) { magic = 0x534F414Bu; boots = rounds_total = 0; }
    boots++;
    sys_crash_t c;
    if (sys_last_crash(&c)) printf("soak: CRASH before this boot: \"%s\" pc 0x%08lx\n", c.why, c.pc);
    printf("soak: boot %lu, reset cause %s, rounds so far %lu\n", boots, sys_reset_cause_str(sys_reset_cause(0)), rounds_total);
    uint32_t ps = psram_init(2);
    tsens_init();
    static bdev_t bd; static wl_t wl;
    int fs = wl_mount(&wl, &bd, partition_find(PART_DATA, PART_SUB_FAT, "fat"));
    if (!fs) fs = fat_format(&bd) || fat_mount(&bd);          /* the soak owns this partition: fresh volume per boot */
    printf("soak: fs %d, psram %lu\n", fs, ps);
    event_on(1, on_ev, 0);
    for (uint32_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31 + (i >> 8));
    uint8_t ref[32]; sha(SHA256, buf, sizeof buf, ref);
    wdt_start(10000, true);
    uint32_t fails = 0;
    for (uint32_t r = 0;; r++) {
        uint32_t t0 = time_ms(); int bad = 0;
        uint8_t d[32]; sha(SHA256, buf, sizeof buf, d); bad |= memcmp(d, ref, 32) ? 1 : 0;
        uint8_t key[32], iv[12], tag[16]; rng_fill(key, 32); rng_fill(iv, 12);
        aes_gcm(key, 32, true, iv, 12, 0, 0, buf, out, sizeof out, tag);
        int vr = aes_gcm(key, 32, false, iv, 12, 0, 0, out, dec, sizeof out, tag);   /* tag in: verified */
        bad |= vr || memcmp(dec, buf, sizeof dec) ? 2 : 0;
        if (ps) {
            uint32_t *p = (uint32_t *)PSRAM_VADDR, n = (ps < (4u << 20) ? ps : (4u << 20)) / 4, seed = rng_u32();
            for (uint32_t i = 0; i < n; i++) p[i] = seed ^ (i * 2654435761u);
            psram_writeback(p, n * 4); psram_invalidate(p, n * 4);
            for (uint32_t i = 0; i < n; i++) if (p[i] != (seed ^ (i * 2654435761u))) { bad |= 4; break; }
        }
        if (!fs) {
            int fd = fat_open("/soak.bin", FAT_WRONLY | FAT_CREAT | FAT_TRUNC);
            int wn = fat_write(fd, buf + (r & 255), 32768), wc = fat_close(fd);
            if (wn != 32768 || wc) bad |= 8;
            fd = fat_open("/soak.bin", FAT_RDONLY); int k = fat_read(fd, out, 16384); k += fat_read(fd, out, 16384); fat_close(fd);
            fd = fat_open("/soak.bin", FAT_RDONLY); uint32_t crc = 0; for (int j = 0; j < 2; j++) { fat_read(fd, out, 16384); crc = crc32_le(crc, out, 16384); } fat_close(fd);
            bad |= k != 32768 || crc != crc32_le(0, buf + (r & 255), 32768) ? 8 : 0;
        }
        ev_n = 0; for (int i = 0; i < 20; i++) event_post(1, 1); event_dispatch(); bad |= ev_n != 20 ? 16 : 0;
        co_n = 0; coop_spawn(task, 0, st[0], sizeof st[0]); coop_spawn(task, 0, st[1], sizeof st[1]); coop_run(); bad |= co_n != 100 ? 32 : 0;
        float tc = tsens_read_c();
        wdt_feed();
        rounds_total++; fails += bad != 0;
        printf("soak: round %lu %s (mask %d) %lu ms, total %lu, fails %lu, %d.%d C, heap %u\n", r, bad ? "FAIL" : "PASS", bad,
               time_ms() - t0, rounds_total, fails, (int)tc, (int)(tc * 10) % 10, (unsigned)heap_free());
    }
}
