/* MQTT 3.1.1 packet codec (OASIS mqtt-v3.1.1 section 2-3): pure, every length checked. */
#include <string.h>
#include "mqtt.h"

unsigned mqtt_varint(uint8_t *o, uint32_t v) { unsigned n = 0; do { uint8_t b = v & 127; v >>= 7; o[n++] = (uint8_t)(b | (v ? 128 : 0)); } while (v && n < 4); return n; }

int mqtt_fixed(const uint8_t *p, unsigned n, uint8_t *tf, uint32_t *remlen)
{
    if (n < 2) return 0;
    uint32_t v = 0; unsigned i = 1;
    for (;; i++) {
        if (i > 4) return -1;                                          /* at most 4 length bytes */
        if (i >= n) return 0;
        v |= (uint32_t)(p[i] & 127) << (7 * (i - 1));
        if (!(p[i] & 128)) break;
    }
    if (i > 1 && !(p[i] & 127)) return -1;                             /* non-minimal (trailing zero digit) */
    *tf = p[0]; *remlen = v;
    return (int)i + 1;
}

int mqtt_parse_publish(const uint8_t *p, unsigned n, uint8_t flags, const uint8_t **topic, unsigned *tl, uint16_t *pid, const uint8_t **pay, unsigned *pl)
{
    int qos = (flags >> 1) & 3;
    if (qos == 3 || n < 2) return -1;
    unsigned l = (unsigned)(p[0] << 8 | p[1]);
    if (!l || 2 + l > n) return -1;                                    /* a topic name is required */
    for (unsigned i = 0; i < l; i++) if (p[2 + i] == '+' || p[2 + i] == '#' || !p[2 + i]) return -1;   /* no wildcards / NUL in a published topic */
    unsigned o = 2 + l;
    if (qos) { if (o + 2 > n) return -1; *pid = (uint16_t)(p[o] << 8 | p[o + 1]); if (!*pid) return -1; o += 2; } else *pid = 0;
    *topic = p + 2; *tl = l; *pay = p + o; *pl = n - o;
    return 0;
}

static unsigned str(uint8_t *o, const char *s, unsigned l) { o[0] = (uint8_t)(l >> 8); o[1] = (uint8_t)l; memcpy(o + 2, s, l); return 2 + l; }

unsigned mqtt_build_connect(uint8_t *o, unsigned max, const char *id, const mqtt_cfg_t *c)
{
    unsigned il = (unsigned)strlen(id), ul = c->user ? (unsigned)strlen(c->user) : 0, pl = c->pass ? (unsigned)strlen(c->pass) : 0;
    unsigned wt = c->will_topic ? (unsigned)strlen(c->will_topic) : 0;
    uint32_t rem = 10 + 2 + il + (wt ? 2 + wt + 2 + c->will_len : 0) + (c->user ? 2 + ul : 0) + (c->user && c->pass ? 2 + pl : 0);
    if (il > 23 || 5 + rem > max || wt > 1000 || c->will_len > 1000) return 0;
    unsigned n = 0; o[n++] = 0x10; n += mqtt_varint(o + n, rem);
    n += str(o + n, "MQTT", 4); o[n++] = 4;                            /* protocol level 4 = 3.1.1 */
    uint8_t f = 0x02;                                                  /* clean session */
    if (wt) f |= (uint8_t)(0x04 | (c->will_qos & 1) << 3 | (c->will_retain ? 0x20 : 0));
    if (c->user) { f |= 0x80; if (c->pass) f |= 0x40; }
    o[n++] = f; uint16_t ka = c->keepalive ? c->keepalive : 120; o[n++] = (uint8_t)(ka >> 8); o[n++] = (uint8_t)ka;
    n += str(o + n, id, il);
    if (wt) { n += str(o + n, c->will_topic, wt); n += str(o + n, (const char *)c->will_msg, c->will_len); }
    if (c->user) { n += str(o + n, c->user, ul); if (c->pass) n += str(o + n, c->pass, pl); }
    return n;
}

unsigned mqtt_build_publish(uint8_t *o, unsigned max, const char *topic, const void *p, unsigned n, int qos, int retain, int dup, uint16_t pid)
{
    unsigned tl = (unsigned)strlen(topic); uint32_t rem = 2 + tl + (qos ? 2 : 0) + n;
    if (!tl || tl > 65535 || 5 + rem > max) return 0;
    unsigned k = 0; o[k++] = (uint8_t)(0x30 | (dup ? 8 : 0) | (qos ? 2 : 0) | (retain ? 1 : 0)); k += mqtt_varint(o + k, rem);
    k += str(o + k, topic, tl);
    if (qos) { o[k++] = (uint8_t)(pid >> 8); o[k++] = (uint8_t)pid; }
    memcpy(o + k, p, n);
    return k + n;
}

unsigned mqtt_build_subscribe(uint8_t *o, unsigned max, uint16_t pid, const char *topic, int qos, int unsub)
{
    unsigned tl = (unsigned)strlen(topic); uint32_t rem = 2 + 2 + tl + (unsub ? 0 : 1);
    if (!tl || 5 + rem > max) return 0;
    unsigned k = 0; o[k++] = unsub ? 0xA2 : 0x82; k += mqtt_varint(o + k, rem);    /* reserved flags 0010 */
    o[k++] = (uint8_t)(pid >> 8); o[k++] = (uint8_t)pid; k += str(o + k, topic, tl);
    if (!unsub) o[k++] = (uint8_t)(qos ? 1 : 0);
    return k;
}
