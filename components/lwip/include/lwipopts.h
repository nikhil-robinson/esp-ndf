/* lwIP configuration for esp-ndf: NO_SYS, raw API only, IPv4. Sizes come from NDF_NET_* (ndf_config.h) with these defaults. */
#pragma once
#include <stdint.h>
#include "rng.h"

#ifndef NDF_NET_MEM_SIZE
#define NDF_NET_MEM_SIZE   (24 * 1024)       /* lwIP heap: PCBs, TCP segments, outgoing pbufs */
#endif
#ifndef NDF_NET_PBUFS
#define NDF_NET_PBUFS      8                 /* receive buffers of 1536 B */
#endif
#ifndef NDF_NET_TCP_SEGS
#define NDF_NET_TCP_SEGS   6                 /* TCP window and send buffer in segments */
#endif

#define NO_SYS                      1
#define LWIP_TIMERS                 1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_NETIF_API              0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_BROADCAST_PING         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_SINGLE_NETIF           1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    NDF_NET_MEM_SIZE
#define MEMP_NUM_PBUF               16
#define PBUF_POOL_SIZE              NDF_NET_PBUFS
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_UDP_PCB            6
#define MEMP_NUM_TCP_PCB            6
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            (4 * NDF_NET_TCP_SEGS)
#define MEMP_NUM_RAW_PCB            2
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define ARP_QUEUEING                1

#ifndef NDF_NET_TCP_MSS
#define NDF_NET_TCP_MSS            536               /* 1 Mbit/s downlink: 1460-byte frames last 12 ms and collide with our ACKs (measured) */
#endif
#define TCP_MSS                     NDF_NET_TCP_MSS
#define TCP_WND                     (NDF_NET_TCP_SEGS * TCP_MSS)
#define TCP_SND_BUF                 (NDF_NET_TCP_SEGS * TCP_MSS)
#define TCP_SND_QUEUELEN            (2 * NDF_NET_TCP_SEGS + 2)
#define LWIP_TCP_KEEPALIVE          1
#define DNS_TABLE_SIZE              4

#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   1
#define MEMP_STATS                  1
#define SYS_STATS                   0
#define LINK_STATS                  1
#define IP_STATS                    1
#define ICMP_STATS                  1
#define UDP_STATS                   1
#define TCP_STATS                   1
#define ETHARP_STATS                1
#define IPFRAG_STATS                0
#define IGMP_STATS                  0
#define MIB2_STATS                  0

#define LWIP_CHECKSUM_ON_COPY       0
#define LWIP_RAND()                 ((uint32_t)rng_u32())
#define LWIP_DEBUG                  0

/* the tree is the IDF copy: its opt.h reads these ESP additions; off = plain lwIP timers */
#define ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND 0
#define ESP_LWIP_IGMP_TIMERS_ONDEMAND      0
#define ESP_LWIP_DNS_TIMERS_ONDEMAND       0
#define ESP_LWIP_MLD6_TIMERS_ONDEMAND      0
