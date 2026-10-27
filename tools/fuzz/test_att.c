/* Host test + fuzzer of the ATT server (components/ble_host/att.c): protocol vectors, then random PDUs under ASan / UBSan.
 * usage: test_att [fuzz rounds] [seed] */
#include "gatt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t stored[16]; static int stored_n;
static int rd1(uint8_t *o, uint16_t max) { for (int i = 0; i < 16 && i < max; i++) o[i] = (uint8_t)i; return max < 16 ? max : 16; }
static int wr1(const uint8_t *in, uint16_t n) { stored_n = n > 16 ? 16 : n; memcpy(stored, in, stored_n); return 0; }
static int rd2(uint8_t *o, uint16_t max) { o[0] = 0x77; return 1; }
static gatt_chr_t chrs[2] = { { 0xFFF1, GATT_P_READ | GATT_P_WRITE, rd1, wr1, 0 }, { 0xFFF2, GATT_P_READ | GATT_P_NOTIFY, rd2, 0, 0 } };

static int fails;
static void t(const char *name, const char *req_hex, const char *want_hex)
{
    uint8_t q[64], got[64], r[GATT_MTU + 8];
    unsigned n = 0, w = 0;
    char rq[160], wt[160]; { int a = 0; for (const char *c = req_hex; *c; c++) if (*c != ' ') rq[a++] = *c; rq[a] = 0; a = 0; for (const char *c = want_hex; *c; c++) if (*c != ' ') wt[a++] = *c; wt[a] = 0; req_hex = rq; want_hex = wt; }
    for (const char *h = req_hex; h[0] && h[1]; h += 2, n++) { char b[3] = { h[0], h[1], 0 }; q[n] = (uint8_t)strtoul(b, 0, 16); }
    int rl = att_handle(q, n, r, sizeof r);
    for (const char *h = want_hex; h[0] && h[1]; h += 2, w++) { char b[3] = { h[0], h[1], 0 }; got[w] = (uint8_t)strtoul(b, 0, 16); }
    if (rl != (int)w || memcmp(r, got, w)) {
        fails++; printf("FAIL %s: got", name); for (int i = 0; i < rl; i++) printf(" %02x", r[i]); printf(", want %s\n", want_hex);
    }
}

int main(int argc, char **argv)
{
    gatt_db_clear();
    if (gatts_add_service(0xFFF0, chrs, 2)) { puts("FAIL add_service"); return 1; }
    if (chrs[0].handle != 3 || chrs[1].handle != 5) { printf("FAIL handles %u %u\n", chrs[0].handle, chrs[1].handle); return 1; }
    t("mtu", "02f700", "031700");
    t("services", "100100ffff0028", "11060100060 0f0ff");
    t("chars", "080100ffff0328", "0907020 00a0300f1ff0400120500f2ff");
    t("find info", "0405000600", "05010500f2ff06000229");
    t("find by type value", "060100ffff0028f0ff", "07010006 00");
    t("read value", "0a0300", "0b000102030405060708090a0b0c0d0e0f");
    t("read blob", "0c03000200", "0d02030405060708090a0b0c0d0e0f");
    t("read blob past end", "0c03001100", "010c030007");
    t("read cccd (off)", "0a0600", "0b0000");
    t("write cccd", "12060001 00", "13");
    t("read cccd (on)", "0a0600", "0b0100");
    t("write value", "120300dead", "13");
    if (stored_n != 2 || stored[0] != 0xde || stored[1] != 0xad) { fails++; puts("FAIL write callback data"); }
    t("write cccd bad bit", "12060002 00", "01120600 03");
    t("write cccd bad len", "120600 01", "0112060 00d");
    t("write read-only value", "120500aa", "011205 0003");
    t("write command bad handle", "52090001", "");
    t("write command ok", "520300beef", "");
    t("missing handle", "0a0900", "010a090001");
    t("handle 0", "0a0000", "010a000001");
    t("start > end", "08050001000328", "0108050001");
    t("short read", "0a03", "010a000004");
    t("unknown request", "30", "01300000 06");
    t("unknown command", "7f0102", "");
    t("empty pdu", "", "");
    t("prepare write", "160300 0000aa", "0116000006");
    t("128-bit uuid by type", "08010 0ffff" "00112233445566778899aabbccddeeff", "0108010 00a");
    t("bad group type", "100100ffff0128", "011001 0010");
    if (fails) { printf("%d vector failures\n", fails); return 1; }
    puts("vectors ok");

    unsigned long rounds = argc > 1 ? strtoul(argv[1], 0, 0) : 0, x = argc > 2 ? strtoul(argv[2], 0, 0) : 7;
    static const uint8_t ops[] = { 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x10, 0x12, 0x52, 0x1E, 0x16, 0x18, 0x01, 0x20, 0xD2 };
    for (unsigned long i = 0; i < rounds; i++) {
        uint8_t q[48], r[GATT_MTU + 8];
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        unsigned n = (unsigned)(x % 40), k;
        for (k = 0; k < n; k++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; q[k] = (uint8_t)x; if ((x & 0x300) == 0) q[k] &= 0x0F; }
        if (n && (x & 0xC00)) q[0] = ops[(x >> 12) % sizeof ops];
        memset(r, 0xA5, sizeof r);
        int rl = att_handle(q, n, r, GATT_MTU);
        if (rl < 0 || rl > GATT_MTU) { printf("FAIL fuzz: response length %d\n", rl); return 1; }
        for (unsigned j = GATT_MTU; j < sizeof r; j++) if (r[j] != 0xA5) { puts("FAIL fuzz: wrote past the response buffer"); return 1; }
        if ((i & 0xFFF) == 0) gatt_reset();
    }
    printf("fuzz_att: %lu PDUs, no crash\n", rounds);
    return 0;
}
