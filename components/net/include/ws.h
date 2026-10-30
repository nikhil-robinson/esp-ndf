/* WebSocket (RFC 6455) over lwIP raw TCP: a server on the HTTP server's port (ws_route: GET <path> with Upgrade) and a client (ws_connect,
 * ws:// by IP address). Messages up to NDF_WS_MSG_MAX bytes (fragmented ones are joined), ping answered, close echoed. Runs from net_poll().
 * The frame codec (ws_frame_*) is pure: the host tests and the fuzzer drive it. Not done: wss://, permessage-deflate, UTF-8 checks on text. */
#pragma once
#include <stdint.h>

#ifndef NDF_WS_MSG_MAX
#define NDF_WS_MSG_MAX 4096
#endif
#ifndef NDF_WS_CONNS
#define NDF_WS_CONNS 2
#endif
enum { WS_CONT = 0, WS_TEXT = 1, WS_BIN = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };
#define WS_CLIENT (-1)                                              /* the id of our one client connection */

typedef void (*ws_msg_cb)(int id, int op, const uint8_t *p, unsigned n);   /* a whole text / binary message; op WS_CLOSE (n = 0) when the connection ends */

int ws_route(const char *path, ws_msg_cb cb);                      /* server: accept upgrades on this path (http_server_start runs the port); 0 ok */
int ws_connect(const char *url, ws_msg_cb cb, uint32_t timeout_ms);   /* client: "ws://a.b.c.d[:port]/path"; 0 open, -1 failed (connect, status, Sec-WebSocket-Accept) */
int ws_send(int id, int op, const void *p, unsigned n);            /* id: a server connection (as given to cb) or WS_CLIENT; 0 queued */
int ws_close(int id, uint16_t code);                               /* close frame; the connection ends when the peer answers (or after 2 s) */
int ws_open(int id);                                               /* 1 while the connection is open */

/* frame codec: header of a frame at p (n bytes available). Returns the header length (2..14), 0 = need more bytes, -1 = protocol error */
typedef struct { uint8_t fin, op, masked, mask[4]; uint64_t len; } ws_hdr_t;
int      ws_frame_parse(const uint8_t *p, unsigned n, ws_hdr_t *h);
unsigned ws_frame_head(uint8_t *o, int fin, int op, uint64_t len, const uint8_t *mask);   /* writes the header (2..14 bytes); mask NULL = unmasked */
void     ws_mask(uint8_t *p, unsigned n, const uint8_t mask[4], unsigned offset);
void     ws_accept_key(const char *key, char out[29]);              /* base64(SHA-1(key | 258EAFA5-E914-47DA-95CA-C5AB0DC85B11)) */
