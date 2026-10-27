#pragma once
/* Command interface of the peer: each handler prints output lines and returns 0 (-> "ok") or < 0 (-> "err <code>"). */
typedef int (*peer_cmd_fn)(int argc, char **argv);
typedef struct { const char *name; peer_cmd_fn fn; } peer_cmd_t;
extern const peer_cmd_t cmds_ble[], cmds_wifi[], cmds_net[];
int hex2bin(const char *h, unsigned char *out, int max);
