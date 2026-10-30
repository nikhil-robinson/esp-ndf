/* MQTT codec (components/net/mqtt_pkt.c) under ASan / UBSan: remaining-length examples of the spec (0, 127, 128, 16383, 16384, 2097151,
 * 268435455), malformed lengths, every truncation of built packets, PUBLISH parse rules, random bytes. usage: fuzz_mqtt <rounds> <seed> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 100000; srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    static const uint32_t v[] = { 0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455 }; static const unsigned vl[] = { 1, 1, 2, 2, 3, 3, 4, 4 };
    for (unsigned i = 0; i < 8; i++) {
        uint8_t o[8] = { 0x30 }; unsigned n = mqtt_varint(o + 1, v[i]); uint8_t tf; uint32_t r;
        CHECK(n == vl[i] && mqtt_fixed(o, 1 + n, &tf, &r) == (int)(1 + n) && r == v[i] && tf == 0x30);
        for (unsigned c = 0; c < 1 + n; c++) CHECK(mqtt_fixed(o, c, &tf, &r) == 0);
    }
    uint8_t bad[6] = { 0x30, 0xff, 0xff, 0xff, 0xff, 0x01 }; uint8_t tf; uint32_t r;
    CHECK(mqtt_fixed(bad, 6, &tf, &r) == -1);                               /* 5 length bytes */
    uint8_t nm[3] = { 0x30, 0x80, 0x00 }; CHECK(mqtt_fixed(nm, 3, &tf, &r) == -1);   /* non-minimal */
    uint8_t p[300]; const uint8_t *t, *pay; unsigned tl, pl; uint16_t pid; int hl;
    unsigned n = mqtt_build_publish(p, sizeof p, "a/b", "xyz", 3, 1, 1, 0, 7);
    hl = mqtt_fixed(p, n, &tf, &r);
    CHECK(hl == 2 && tf == 0x33 && hl + r == n && !mqtt_parse_publish(p + hl, r, tf & 15, &t, &tl, &pid, &pay, &pl) && tl == 3 && !memcmp(t, "a/b", 3) && pid == 7 && pl == 3 && !memcmp(pay, "xyz", 3));
    for (unsigned c = 0; c < r; c++) { int x = mqtt_parse_publish(p + hl, c, tf & 15, &t, &tl, &pid, &pay, &pl); CHECK(x == -1 || (pl <= c && t + tl <= p + hl + c)); }
    n = mqtt_build_publish(p, sizeof p, "a/#", "x", 1, 0, 0, 0, 0); hl = mqtt_fixed(p, n, &tf, &r); CHECK(mqtt_parse_publish(p + hl, r, tf & 15, &t, &tl, &pid, &pay, &pl) == -1);   /* wildcard */
    n = mqtt_build_publish(p, sizeof p, "a", "x", 1, 1, 0, 0, 0); hl = mqtt_fixed(p, n, &tf, &r); CHECK(mqtt_parse_publish(p + hl, r, tf & 15, &t, &tl, &pid, &pay, &pl) == -1);   /* packet id 0 */
    CHECK(mqtt_parse_publish(p + hl, r, 6, &t, &tl, &pid, &pay, &pl) == -1);   /* QoS 3 */
    mqtt_cfg_t c = { .user = "u", .pass = "pw", .will_topic = "w", .will_msg = (const uint8_t *)"bye", .will_len = 3, .will_qos = 1, .keepalive = 30 };
    n = mqtt_build_connect(p, sizeof p, "id", &c); hl = mqtt_fixed(p, n, &tf, &r);
    CHECK(n && hl + r == n && tf == 0x10 && p[hl + 6] == 4 && p[hl + 7] == (0x80 | 0x40 | 0x04 | 0x08 | 0x02) && p[hl + 8] == 0 && p[hl + 9] == 30);
    CHECK(mqtt_build_connect(p, 20, "id", &c) == 0);                        /* does not fit */
    n = mqtt_build_subscribe(p, sizeof p, 9, "x/+", 1, 0); CHECK(n == 2 + 2 + 2 + 3 + 1 && p[0] == 0x82 && p[n - 1] == 1);
    for (unsigned k = 0; k < rounds; k++) {
        uint8_t g[64]; unsigned gn = (unsigned)(rand() % 65); for (unsigned i = 0; i < gn; i++) g[i] = (uint8_t)rand();
        int x = mqtt_fixed(g, gn, &tf, &r); CHECK(x >= -1 && x <= 5 && x <= (int)gn);
        if (gn > 2) { int y = mqtt_parse_publish(g + 1, gn - 1, g[0] & 15, &t, &tl, &pid, &pay, &pl); if (!y) CHECK(pay + pl == g + gn && t >= g + 1); }
    }
    printf("fuzz_mqtt: %u random packets, %s\n", rounds, fails ? "FAILED" : "no memory error, all rules held");
    return fails ? 1 : 0;
}
