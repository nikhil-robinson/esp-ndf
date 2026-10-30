/* HTTPS on lwIP raw TCP: the HTTP server of http.c and a blocking client, over the TLS 1.3 engine of components/tls. Routes are shared with http_route().
 * Everything runs from net_poll(). One server connection and one client at a time (a tls_t is about 10 KB). */
#pragma once
#include <stdint.h>
#include "http.h"
#include "tls.h"

int  https_server_start(uint16_t port, const tls_server_cfg_t *cfg);                      /* cfg (certificate, key) must stay valid; 0 ok */
void https_server_stop(void);
int  https_tls_error(void);                                /* the last connection's TLS error (TLS_ERR_*), 0 = none */
int  https_tls_version(void);                              /* the last connection's TLS version (0x0303 / 0x0304), 0 if it did not connect */
int  https_get(const char *url, const tls_client_cfg_t *cfg, uint8_t *body, unsigned max, int *status);   /* "https://a.b.c.d[:port]/path"; cfg->host is sent as SNI and checked against the certificate; body length received, -1 on error or when the whole request takes over HTTPS_TIMEOUT_MS (90 s) */
