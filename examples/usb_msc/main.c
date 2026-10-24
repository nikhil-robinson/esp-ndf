/* USB mass storage: a 128 KB RAM disk formatted FAT (serial 1234-ABCD) with HELLO.TXT, served for an OTG window of
 * `secs` seconds on "msc <secs>"; "hid <secs>" runs a 64-byte vendor HID instead (console on USB Serial/JTAG before and after). Host side: tools/usb_msc_test.py. */
#include "console.h"
#include "sys.h"
#include "usb.h"
#include "fat.h"
#include <string.h>
#include <stdlib.h>

#define NBLK 256
static uint8_t ram[NBLK * 512] __attribute__((aligned(4)));
static int rd(bdev_t *b, uint32_t s, void *p, uint32_t n) { (void)b; memcpy(p, ram + s * 512, n * 512); return 0; }
static int wr(bdev_t *b, uint32_t s, const void *p, uint32_t n) { (void)b; memcpy(ram + s * 512, p, n * 512); return 0; }
static bdev_t disk = { .sector_size = 512, .sector_count = NBLK, .read = rd, .write = wr };

#define LAST ((volatile uint32_t *)0x50001000u)   /* RTC slow memory: counters of the last window */

static void msc_run(uint32_t secs)
{
    printf("msc: PHY to OTG for %lu s\n", (unsigned long)secs); delay_ms(50);
    usb_dev_function(USB_FN_MSC);
    usb_msc_attach(&disk, false);
    int rc = usb_dev_start();
    uint32_t t0 = time_ms();
    while (rc == 0 && time_ms() - t0 < secs * 1000) { usb_dev_poll(); usb_msc_poll(); }
    usb_dev_stop();
    usb_msc_stats_t s; usb_msc_stats(&s);
    LAST[0] = 0x4D534331u; LAST[1] = (uint32_t)rc; LAST[2] = s.cmds; LAST[3] = s.read_blocks; LAST[4] = s.write_blocks; LAST[5] = s.errors;
    delay_ms(1500);
    printf("msc rc %d cmds %lu read %lu write %lu errors %lu\n", rc, s.cmds, s.read_blocks, s.write_blocks, s.errors);
}

static void hid_run(uint32_t secs)
{
    printf("hid: PHY to OTG for %lu s\n", (unsigned long)secs); delay_ms(50);
    usb_dev_function(USB_FN_HID);
    int rc = usb_dev_start();
    uint32_t t0 = time_ms(), sent = 0, got = 0, last = 0; uint8_t r[64];
    while (rc == 0 && time_ms() - t0 < secs * 1000) {
        usb_dev_poll();
        if (usb_hid_recv(r)) got++;
        if (time_ms() - last >= 10) { memset(r, 0, 64); r[0] = (uint8_t)sent; if (usb_hid_send(r)) { sent++; last = time_ms(); } }
    }
    usb_dev_stop();
    LAST[0] = 0x48494431u; LAST[1] = (uint32_t)rc; LAST[2] = sent; LAST[3] = got;
    delay_ms(1500);
    printf("hid rc %d sent %lu got %lu\n", rc, sent, got);
}

int main(void)
{
    int f = fat_format(&disk), m = fat_mount(&disk);
    int fd = fat_open("/HELLO.TXT", FAT_WRONLY | FAT_CREAT);
    fat_write(fd, "hello over USB mass storage\n", 28); fat_close(fd); fat_unmount();
    if (LAST[0] == 0x48494431u) { printf("last hid: rc %ld sent %lu got %lu\n", (long)LAST[1], LAST[2], LAST[3]); LAST[0] = 0; }
    if (LAST[0] == 0x4D534331u) {
        printf("last msc: rc %ld cmds %lu read %lu write %lu errors %lu\n", (long)LAST[1], LAST[2], LAST[3], LAST[4], LAST[5]);
        LAST[0] = 0;
    }
    printf("usb_msc ready: FAT %d/%d, 'msc <secs>'\n", f, m);
    char line[32]; int n = 0;
    for (;;) {
        int c = console_getc();
        if (c < 0) continue;
        if (c == '\n' || c == '\r') {
            line[n] = 0; n = 0;
            if (!strncmp(line, "msc ", 4)) msc_run((uint32_t)atoi(line + 4));
            else if (!strncmp(line, "hid ", 4)) hid_run((uint32_t)atoi(line + 4));
            else if (line[0]) printf("ok\n");
        } else if (n < 31) line[n++] = (char)c;
    }
}
