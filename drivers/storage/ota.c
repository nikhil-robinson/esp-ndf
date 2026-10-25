/* OTA, ESP-IDF format (app_update/esp_ota_ops.c, bootloader_common_loader.c): otadata = 2 select entries, one per
 * 4 KB sector {seq, label[20], state, crc32_le(~0, seq)}; the highest valid seq boots slot (seq - 1) % slots. New
 * boot choice = next seq that maps to the slot, written to the sector not holding the active entry. */
#include "ota.h"
#include "flash.h"
#include "sha.h"
#include "p256.h"
#include "crc32.h"
#include <string.h>

typedef struct { uint32_t seq; uint8_t label[20]; uint32_t state; uint32_t crc; } sel_t;
#define OTA_NOTE 0x50001FF0u          /* loader: "OTAS", image offset, XIP offset */

static const partition_t *otadata(void) { partition_load(); return partition_find(PART_DATA, 0x00, NULL); }
static int slots(const partition_t **s)
{
    int n = 0;
    for (int i = 0; i < 16; i++) { const partition_t *p = partition_find(PART_APP, 0x10 + i, NULL); if (p) s[n++] = p; }
    return n;
}
static bool sel_ok(const sel_t *e)
{
    return e->seq != 0xFFFFFFFFu && e->state != OTA_INVALID && e->state != OTA_ABORTED &&
           e->crc == crc32_le(0xFFFFFFFFu, &e->seq, 4);
}
static int active(const partition_t *od, sel_t e[2])              /* index of the active entry, -1 if none */
{
    partition_read(od, 0, &e[0], sizeof e[0]); partition_read(od, 0x1000, &e[1], sizeof e[1]);
    int a = -1;
    for (int k = 0; k < 2; k++) if (sel_ok(&e[k]) && (a < 0 || e[k].seq > e[a].seq)) a = k;
    return a;
}
static int sel_put(const partition_t *od, int k, sel_t *e)
{
    REG(0x60008038u) |= 1u << 13;      /* next reset through the ROM + loader: it re-reads otadata (no warm stage) */
    e->crc = crc32_le(0xFFFFFFFFu, &e->seq, 4);
    if (partition_erase(od, 0x1000 * k, 0x1000)) return -1;
    return partition_write(od, 0x1000 * k, e, sizeof *e);
}

const partition_t *ota_running(void)
{
    partition_load();
    uint32_t off = REG(OTA_NOTE) == 0x5341544Fu ? REG(OTA_NOTE + 4) : NDF_BOOT_LOAD_ADDR;
    for (int i = 0; ; i++) {
        const partition_t *p = partition_get(i);
        if (!p) return NULL;
        if (p->type == PART_APP && off >= p->offset && off < p->offset + p->size) return p;
    }
}

const partition_t *ota_next(void)
{
    const partition_t *s[16], *r = ota_running();
    int n = slots(s);
    if (!n) return NULL;
    for (int i = 0; i < n; i++) if (s[i] == r) return s[(i + 1) % n];
    return s[0];
}

static const partition_t *verified;                 /* the slot ota_end last accepted */

int ota_begin(ota_t *o, const partition_t *slot)
{
    verified = NULL;
    if (!slot || slot->type != PART_APP || slot == ota_running()) return -1;
    o->part = slot; o->written = 0;
    return partition_erase(slot, 0, slot->size);
}

int ota_write(ota_t *o, const void *data, size_t len)
{
    if (o->written > o->part->size || len > o->part->size - o->written) return -1;
    if (partition_write(o->part, o->written, data, len)) return -1;
    o->written += len;
    return 0;
}

/* signed trailer (tools/ota_sign.py): "NDFS", version, length of the image before it, ECDSA P-256 r||s over
 * SHA-256(image || "NDFS" || version || length). Covers every written byte: RAM image and code window. */
#define SIG_MAGIC 0x5346444Eu
static uint8_t ota_pub[64]; static bool ota_has_key; static uint32_t ota_min_ver;

int ota_set_key(const uint8_t pub[64], uint32_t min_version)
{
    if (p256_check_pub(pub)) return -1;
    memcpy(ota_pub, pub, 64); ota_has_key = true; ota_min_ver = min_version;
    return 0;
}

static int hash_range(const partition_t *p, uint32_t len, sha_ctx_t *c)
{
    uint8_t b[256];
    for (uint32_t a = 0; a < len; a += sizeof b) {
        uint32_t n = len - a < sizeof b ? len - a : sizeof b;
        if (partition_read(p, a, b, n)) return -1;
        sha_update(c, b, n);
    }
    return 0;
}

static int check_signature(ota_t *o, uint32_t *img_len)
{
    if (o->written < 76) return -1;
    uint32_t t[3]; uint8_t sig[64];
    if (partition_read(o->part, o->written - 76, t, 12) || partition_read(o->part, o->written - 64, sig, 64)) return -1;
    if (t[0] != SIG_MAGIC || t[2] != o->written - 76 || t[1] < ota_min_ver) return -1;   /* also: no downgrade */
    sha_ctx_t c; sha_start(&c, SHA256);
    if (hash_range(o->part, t[2], &c)) return -1;
    sha_update(&c, t, 12);
    uint8_t d[32]; sha_finish(&c, d);
    if (p256_verify(ota_pub, d, sig)) return -1;
    *img_len = t[2];
    return 0;
}

int ota_end(ota_t *o)                   /* signature (when required), then the RAM image: header, segments, SHA-256 */
{
    verified = NULL;
    uint32_t img = o->written;
    if (NDF_OTA_REQUIRE_SIG || ota_has_key) {
        if (!ota_has_key || check_signature(o, &img)) return -1;
    }
    uint8_t h[24], b[256]; uint32_t off = 24;
    if (partition_read(o->part, 0, h, sizeof h) || h[0] != 0xE9 || !h[23]) return -1;   /* SHA-256 appended: required */
    for (int s = 0; s < h[1]; s++) {
        uint32_t sh[2];
        if (partition_read(o->part, off, sh, 8)) return -1;
        if (sh[1] > img || off + 8 > img - sh[1]) return -1;             /* no wrap: segment inside the image */
        off += 8 + sh[1];
    }
    uint32_t len = (off + 16) & ~15u;
    if (len > img || 32 > img - len) return -1;
    sha_ctx_t c; sha_start(&c, SHA256);
    for (uint32_t a = 0; a < len; a += sizeof b) {
        uint32_t n = len - a < sizeof b ? len - a : sizeof b;
        if (partition_read(o->part, a, b, n)) return -1;
        sha_update(&c, b, n);
    }
    uint8_t d[32], want[32];
    sha_finish(&c, d);
    if (partition_read(o->part, len, want, 32)) return -1;
    uint8_t diff = 0; for (int i = 0; i < 32; i++) diff |= d[i] ^ want[i];
    if (diff) return -1;
    verified = o->part;
    return 0;
}

int ota_set_boot(const partition_t *slot)
{
    const partition_t *od = otadata(), *s[16];
    int n = slots(s), idx = -1;
    for (int i = 0; i < n; i++) if (s[i] == slot) idx = i;
    if (!od || idx < 0) return -1;
    if (slot != verified && slot != ota_running()) return -1;   /* only a slot ota_end accepted (or going back) */
    sel_t e[2]; int a = active(od, e);
    uint32_t seq = a >= 0 ? e[a].seq + 1 : 1;
    while ((seq - 1) % n != (uint32_t)idx) seq++;
    sel_t ne; memset(&ne, 0xFF, sizeof ne);
    ne.seq = seq; ne.state = NDF_OTA_ROLLBACK ? OTA_NEW : 0xFFFFFFFFu;
    return sel_put(od, a >= 0 ? a ^ 1 : 0, &ne);
}

static int running_entry(const partition_t *od, sel_t e[2])     /* the entry the loader booted from, -1 if factory */
{
    if (REG(OTA_NOTE) != 0x5341544Fu || !od) return -1;
    return active(od, e);
}

int ota_state(void)
{
    const partition_t *od = otadata(); sel_t e[2];
    int a = running_entry(od, e);
    return a < 0 ? OTA_UNDEFINED : (int)e[a].state;
}

int ota_mark_valid(void)
{
    const partition_t *od = otadata(); sel_t e[2];
    int a = running_entry(od, e);
    if (a < 0 || e[a].state == OTA_VALID || e[a].state == 0xFFFFFFFFu) return 0;
    e[a].state = OTA_VALID;
    return sel_put(od, a, &e[a]);
}
