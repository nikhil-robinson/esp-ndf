/* DHCP server for the access point's lwIP netif (raw UDP, port 67). One lease per client MAC from a small pool, 1 h lease. Runs from net_poll(). */
#pragma once
#include <stdint.h>
#include "lwip/netif.h"

int  dhcpd_start(struct netif *nif, uint32_t first_ip /* network byte order */, unsigned pool);   /* pool = number of addresses from first_ip upwards; 0 ok */
void dhcpd_stop(void);
unsigned dhcpd_leases(uint32_t *ips, uint8_t macs[][6], unsigned max);                         /* bound leases */
