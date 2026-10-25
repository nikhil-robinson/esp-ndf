/* USB mass storage: a 128 KB RAM disk formatted FAT (serial 1234-ABCD) with HELLO.TXT, served for an OTG window of
 * `secs` seconds on "msc <secs>"; "hid <secs>" runs a 64-byte vendor HID instead (console on USB Serial/JTAG before and after). Host side: tools/usb_msc_test.py. */
#include "console.h"
#include "sys.h"
#include "system.h"
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
    LAST[0] = 0x4D534331u; LAST[1] = (uint32_t)rc; LAST[2] = s.cmds; LAST[3] = s.read_blocks; LAST[4] = s.write_blocks; LAST[5] = s.errors; LAST[6] = s.last_bad_op;
    delay_ms(1500);
    printf("msc rc %d cmds %lu read %lu write %lu errors %lu\n", rc, s.cmds, s.read_blocks, s.write_blocks, s.errors);
}

/* composite: the same disk plus a CDC-ACM echo port, both at once */
static void comp_run(uint32_t secs)
{
    printf("comp: PHY to OTG for %lu s\n", (unsigned long)secs); delay_ms(50);
    usb_dev_function(USB_FN_CDC_MSC);
    usb_msc_attach(&disk, false);
    int rc = usb_dev_start();
    uint32_t t0 = time_ms(), echoed = 0; uint8_t b[256];
    while (rc == 0 && time_ms() - t0 < secs * 1000) {
        usb_dev_poll(); usb_msc_poll();
        size_t n = usb_cdc_read(b, sizeof b);
        for (size_t o = 0; o < n; ) { size_t k = usb_cdc_write(b + o, n - o); if (!k) usb_dev_poll(); o += k; }
        echoed += n;
    }
    usb_dev_stop();
    usb_msc_stats_t s; usb_msc_stats(&s);
    LAST[0] = 0x434F4D31u; LAST[1] = (uint32_t)rc; LAST[2] = s.cmds; LAST[3] = s.read_blocks; LAST[4] = s.errors; LAST[5] = echoed; { usb_stats_t u; usb_get_stats(&u); LAST[6] = u.setups; LAST[7] = u.stalls; LAST[8] = u.rx_bytes; LAST[9] = u.tx_bytes; }
    delay_ms(1500);
    printf("comp rc %d cmds %lu read %lu errors %lu (op %02x) echoed %lu\n", rc, s.cmds, s.read_blocks, s.errors, s.last_bad_op, echoed);
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
    { sys_crash_t k; printf("boot: reset cause %s\n", sys_reset_cause_str(sys_reset_cause(0)));
      if (sys_last_crash(&k)) printf("last crash: %s pc %08lx\n", k.why, k.pc); }
    int f = fat_format(&disk), m = fat_mount(&disk);
    int fd = fat_open("/HELLO.TXT", FAT_WRONLY | FAT_CREAT);
    fat_write(fd, "hello over USB mass storage\n", 28); fat_close(fd); fat_unmount();
    if (LAST[0] == 0x434F4D31u) { printf("comp rc %ld cmds %lu read %lu errors %lu echoed %lu setups %lu stalls %lu rx %lu tx %lu\n", (long)LAST[1], LAST[2], LAST[3], LAST[4], LAST[5], LAST[6], LAST[7], LAST[8], LAST[9]); LAST[0] = 0; }
    if (LAST[0] == 0x48494431u) { printf("last hid: rc %ld sent %lu got %lu\n", (long)LAST[1], LAST[2], LAST[3]); LAST[0] = 0; }
    if (LAST[0] == 0x4D534331u) {
        printf("last msc: rc %ld cmds %lu read %lu write %lu errors %lu (op %02lx)\n", (long)LAST[1], LAST[2], LAST[3], LAST[4], LAST[5], LAST[6]);
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
            else if (!strncmp(line, "comp ", 5)) comp_run((uint32_t)atoi(line + 5));
            else if (line[0]) printf("ok\n");
        } else if (n < 31) line[n++] = (char)c;
    }
}
