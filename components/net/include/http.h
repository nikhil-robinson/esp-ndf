/* HTTP/1.1 over lwIP raw TCP: a small server (routes, keep-alive, fixed connection pool) and a blocking client (http:// by IP address).
 * Everything runs from net_poll(): handlers are called from there and must not block. */
#pragma once
#include <stdint.h>

typedef struct { const char *method, *path; } http_req_t;                     /* valid for the duration of the handler call */
typedef struct { int status; const char *ctype; const uint8_t *body; uint32_t len; } http_resp_t;   /* body must stay valid until the response is sent: static or constant data */

int http_route(const char *method, const char *path, int (*h)(const http_req_t *, http_resp_t *));   /* exact match; 0 ok, -1 table full */
int http_server_start(uint16_t port);                                                /* 0 ok */
void http_server_stop(void);
int http_get(const char *url, uint8_t *body, unsigned max, int *status);           /* "http://a.b.c.d[:port]/path"; body length received (only the first max bytes are stored, the rest is counted), -1 on connect / protocol error or 10 s timeout */
