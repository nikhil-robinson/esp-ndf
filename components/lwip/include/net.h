/* IP over the Wi-Fi station (lwIP, raw API, NO_SYS): net_start() after wifi_sta_join(); net_poll() from the main loop. */
#pragma once
#include <stdint.h>
#include "lwip/netif.h"

int  net_start(const uint8_t mac[6]);                     /* lwIP init, netif on the Wi-Fi station, DHCP client */
void net_poll(void);                                      /* wifi_poll(), lwIP timers (sys_check_timeouts) */
int  net_ip(uint32_t *ip, uint32_t *gw);                  /* 0 once DHCP is bound; addresses in network byte order */
struct netif *net_netif(void);
