/* lwIP settings for the in-title WARP gateway: one thread (NO_SYS), TCP, DNS
 * and the game's UDP, inside a WireGuard tunnel with a 1280-byte MTU. The 1 MiB
 * receive window keeps a connection fast over the ~80 ms round trip to
 * Cloudflare (128 KiB gave 1.3 MB/s); buffers are sized to match. */
#ifndef WARP_LWIPOPTS_H
#define WARP_LWIPOPTS_H

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define SYS_LIGHTWEIGHT_PROT 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_ACD 0
#define LWIP_IGMP 0
#define IP_REASSEMBLY 1
#define IP_FRAG 1

#define MEM_ALIGNMENT 8
#define MEM_SIZE (16 * 1024 * 1024)
#define MEMP_NUM_PBUF 1024
#define PBUF_POOL_SIZE 8192
#define MEMP_NUM_TCP_PCB 256
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG 16384
#define MEMP_NUM_UDP_PCB 80 /* 64 game associations (warp_gateway.c MAX_UDP) + DNS */
#define MEMP_NUM_SYS_TIMEOUT 32
#define MEMP_NUM_REASSDATA 16
#define IP_REASS_MAX_PBUFS 64

#define TCP_MSS 1240 /* 1280 MTU - 20 IP - 20 TCP */
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 5
#define TCP_WND (1024 * 1024)
#define TCP_SND_BUF (256 * 1024)
#define TCP_SND_QUEUELEN 1024
#define TCP_SNDLOWAT (TCP_MSS * 2)
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_KEEPALIVE 1
#define TCP_LISTEN_BACKLOG 0

#define DNS_TABLE_SIZE 128
#define DNS_MAX_NAME_LENGTH 256
#define DNS_MAX_SERVERS 2
#define DNS_MAX_RETRIES 8
#define LWIP_DNS_SECURE (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT)

#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0
#define LWIP_STATS 0
#define LWIP_NOASSERT_ON_ERROR 1

#define LWIP_RAND() ((u32_t)warp_random32())
unsigned warp_random32(void);

#endif
