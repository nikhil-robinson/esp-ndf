/* USB mass storage, bulk-only transport (USB MSC BOT 1.0) + SCSI block commands (SPC/SBC subset Linux, macOS and
 * Windows use), over a bdev_t presented as 512-byte blocks. Runs from usb_msc_poll() in the app loop; data moves through
 * the bulk endpoints of usb_dev.c (usb_cdc_read / usb_cdc_write are the raw bulk calls). Also the HID helpers. */
#include "usb.h"
#include "sys.h"
#include <string.h>

#define BLK 512
static bdev_t *disk; static bool rdonly;
static uint8_t cbw[31]; static uint32_t cbw_n;
static uint8_t sense_key, asc;
static uint8_t io[8192] __attribute__((aligned(4)));     /* staging: 16 blocks per bus transfer */
static usb_msc_stats_t ms;

int usb_msc_attach(bdev_t *b, bool ro) { disk = b; rdonly = ro; cbw_n = 0; return b && b->sector_size >= BLK && b->sector_size % BLK == 0 ? 0 : -1; }
void usb_msc_reset(void) { cbw_n = 0; }
void usb_msc_stats(usb_msc_stats_t *s) { *s = ms; }

static uint32_t blocks(void) { return disk->sector_count * (disk->sector_size / BLK); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void send(const void *p, uint32_t n)               /* bulk IN, waits for room (the OTG guard is fed inside) */
{
    const uint8_t *b = p;
    while (n && usb_cdc_ready()) {
        uint32_t k = usb_cdc_write(b, n);
        if (!k) { usb_dev_poll(); continue; }
        b += k; n -= k;
    }
}
static void recv(void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n && usb_cdc_ready()) { uint32_t k = usb_cdc_read(b, n); if (!k) { usb_dev_poll(); continue; } b += k; n -= k; }
}

/* 512-byte blocks on the device's sectors: whole sectors directly, partial ones read-modify-write */
static int blk_io(uint32_t lba, uint8_t *buf, uint32_t n, bool wr)
{
    uint32_t per = disk->sector_size / BLK;
    if (per == 1) return wr ? disk->write(disk, lba, buf, n) : disk->read(disk, lba, buf, n);
    static uint8_t sec[4096] __attribute__((aligned(4)));
    if (disk->sector_size > sizeof sec) return -1;
    while (n) {
        uint32_t s = lba / per, o = lba % per, k = per - o < n ? per - o : n;
        if (disk->read(disk, s, sec, 1)) return -1;
        if (wr) { memcpy(sec + o * BLK, buf, k * BLK); if (disk->write(disk, s, sec, 1)) return -1; }
        else memcpy(buf, sec + o * BLK, k * BLK);
        lba += k; buf += k * BLK; n -= k;
    }
    return 0;
}

static void csw(uint32_t residue, uint8_t status)
{
    uint8_t c[13];
    put_be32(c, 0); c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'S';
    memcpy(c + 4, cbw + 4, 4);                                  /* tag */
    c[8] = residue; c[9] = residue >> 8; c[10] = residue >> 16; c[11] = residue >> 24;
    c[12] = status;
    send(c, 13);
}

static void fail(uint8_t key, uint8_t a, uint32_t len, bool in)
{
    sense_key = key; asc = a; ms.errors++;
    if (in && len) { memset(io, 0, len < sizeof io ? len : sizeof io); for (uint32_t k = len; k; ) { uint32_t m = k < sizeof io ? k : sizeof io; send(io, m); k -= m; } }
    else if (!in && len) for (uint32_t k = len; k; ) { uint32_t m = k < sizeof io ? k : sizeof io; recv(io, m); k -= m; }
    csw(0, 1);
}

static void command(void)
{
    uint32_t len = cbw[8] | cbw[9] << 8 | cbw[10] << 16 | (uint32_t)cbw[11] << 24;
    bool in = cbw[12] & 0x80;
    const uint8_t *cb = cbw + 15;
    ms.cmds++;
    if (!disk) { fail(0x02, 0x3A, len, in); return; }           /* not ready, medium not present */
    switch (cb[0]) {
    case 0x00: case 0x1E: case 0x1B: case 0x2F: case 0x35:      /* test unit ready, prevent/allow, start/stop, verify, sync */
        sense_key = 0; csw(len, 0); return;
    case 0x03: {                                                /* request sense */
        uint8_t r[18] = { 0x70, 0, sense_key, 0, 0, 0, 0, 10, 0, 0, 0, 0, asc };
        uint32_t n = len < 18 ? len : 18; send(r, n); sense_key = asc = 0; csw(len - n, 0); return;
    }
    case 0x12: {                                                /* inquiry */
        uint8_t r[36] = { 0, 0x80, 4, 2, 31 };                  /* direct access, removable, SPC-2 */
        memcpy(r + 8, "esp-ndf ", 8); memcpy(r + 16, "RAM/flash disk  ", 16); memcpy(r + 32, "1.0 ", 4);
        uint32_t n = len < 36 ? len : 36; send(r, n); csw(len - n, 0); return;
    }
    case 0x1A: {                                                /* mode sense (6) */
        uint8_t r[4] = { 3, 0, rdonly ? 0x80 : 0, 0 };
        uint32_t n = len < 4 ? len : 4; send(r, n); csw(len - n, 0); return;
    }
    case 0x23: {                                                /* read format capacities */
        uint8_t r[12] = { 0, 0, 0, 8 };
        put_be32(r + 4, blocks()); r[8] = 2; r[10] = BLK >> 8;
        uint32_t n = len < 12 ? len : 12; send(r, n); csw(len - n, 0); return;
    }
    case 0x25: {                                                /* read capacity (10) */
        uint8_t r[8]; put_be32(r, blocks() - 1); put_be32(r + 4, BLK);
        uint32_t n = len < 8 ? len : 8; send(r, n); csw(len - n, 0); return;
    }
    case 0x28: case 0x2A: {                                     /* read (10) / write (10) */
        uint32_t lba = be32(cb + 2), cnt = cb[7] << 8 | cb[8];
        bool wr = cb[0] == 0x2A;
        if (lba + cnt > blocks() || cnt * BLK != len) { fail(0x05, 0x21, len, in); return; }   /* LBA out of range */
        if (wr && rdonly) { fail(0x07, 0x27, len, false); return; }                             /* write protected */
        while (cnt) {
            uint32_t k = cnt < sizeof io / BLK ? cnt : sizeof io / BLK;
            if (wr) { recv(io, k * BLK); if (blk_io(lba, io, k, true)) { fail(0x03, 0x0C, (cnt - k) * BLK, false); return; } ms.write_blocks += k; }
            else { if (blk_io(lba, io, k, false)) { fail(0x03, 0x11, cnt * BLK, true); return; } send(io, k * BLK); ms.read_blocks += k; }
            lba += k; cnt -= k;
        }
        if (wr && disk->sync) disk->sync(disk);
        csw(0, 0); return;
    }
    }
    fail(0x05, 0x20, len, in);                                  /* illegal request: invalid command */
}

void usb_msc_poll(void)
{
    if (!usb_cdc_ready()) { cbw_n = 0; return; }
    cbw_n += usb_cdc_read(cbw + cbw_n, sizeof cbw - cbw_n);
    if (cbw_n < sizeof cbw) return;
    cbw_n = 0;
    if (cbw[0] != 'U' || cbw[1] != 'S' || cbw[2] != 'B' || cbw[3] != 'C') return;   /* not a CBW: wait for reset recovery */
    command();
}

/* ---- HID: the same two endpoints carry 64-byte reports ---- */
bool usb_hid_send(const uint8_t r[64]) { return usb_cdc_ready() && usb_cdc_write(r, 64) == 64; }
bool usb_hid_recv(uint8_t r[64]) { return usb_cdc_ready() && usb_cdc_read(r, 64) == 64; }
