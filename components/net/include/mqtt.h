/* MQTT 3.1.1 client over lwIP raw TCP (mqtt://a.b.c.d[:port]): CONNECT (clean session, keep-alive, user / password, will), SUBSCRIBE /
 * UNSUBSCRIBE, PUBLISH QoS 0 and 1 both ways (QoS 1 retransmitted with DUP after NDF_MQTT_RETX_MS, incoming QoS 1 acknowledged; QoS 2 is
 * refused as 1), PINGREQ keep-alive. Defaults as esp-mqtt: keep-alive 120 s, retransmit 1 s, 1024-byte messages. Runs from net_poll().
 * The packet codec (mqtt_pkt.c) is pure: the host tests and the fuzzer drive it. Not done: mqtts://, QoS 2, more than one QoS 1 message in flight. */
#pragma once
#include <stdint.h>

#ifndef NDF_MQTT_BUF
#define NDF_MQTT_BUF 1024              /* largest packet sent or received (topic + payload + headers) */
#endif
#ifndef NDF_MQTT_RETX_MS
#define NDF_MQTT_RETX_MS 1000
#endif

typedef struct {
    const char *client_id;             /* NULL: "esp-ndf-" + 6 random hex digits */
    const char *user, *pass;           /* NULL = none */
    const char *will_topic; const uint8_t *will_msg; unsigned will_len; uint8_t will_qos, will_retain;
    uint16_t keepalive;                /* seconds; 0 = 120 */
} mqtt_cfg_t;
typedef void (*mqtt_msg_cb)(const char *topic, unsigned tl, const uint8_t *p, unsigned n, int qos, int retain);   /* topic not NUL-terminated */

int  mqtt_connect(const char *url, const mqtt_cfg_t *cfg, mqtt_msg_cb cb, uint32_t timeout_ms);   /* 0 connected, -1 no connection / bad CONNACK, 1..5 the broker's refusal code */
int  mqtt_subscribe(const char *topic, int qos, uint32_t timeout_ms);       /* granted QoS (0 / 1), -1 refused / timeout */
int  mqtt_unsubscribe(const char *topic, uint32_t timeout_ms);              /* 0 acknowledged */
int  mqtt_publish(const char *topic, const void *p, unsigned n, int qos, int retain);   /* 0 sent (QoS 1: in flight until PUBACK, see mqtt_inflight), -1 not connected / too big / a QoS 1 message already in flight */
int  mqtt_inflight(void);                                                    /* 1 while our QoS 1 message waits for its PUBACK */
int  mqtt_connected(void);
void mqtt_disconnect(void);                                                  /* DISCONNECT (no will), close */

/* packet codec */
unsigned mqtt_varint(uint8_t *o, uint32_t v);                                 /* remaining-length encoding, 1..4 bytes */
int      mqtt_fixed(const uint8_t *p, unsigned n, uint8_t *type_flags, uint32_t *remlen);   /* header length (2..5), 0 = need more, -1 malformed */
int      mqtt_parse_publish(const uint8_t *p, unsigned n, uint8_t flags, const uint8_t **topic, unsigned *tl, uint16_t *pid, const uint8_t **pay, unsigned *pl);   /* p / n = the variable part; 0 ok */
unsigned mqtt_build_connect(uint8_t *o, unsigned max, const char *id, const mqtt_cfg_t *c);   /* 0 = does not fit */
unsigned mqtt_build_publish(uint8_t *o, unsigned max, const char *topic, const void *p, unsigned n, int qos, int retain, int dup, uint16_t pid);
unsigned mqtt_build_subscribe(uint8_t *o, unsigned max, uint16_t pid, const char *topic, int qos, int unsub);
