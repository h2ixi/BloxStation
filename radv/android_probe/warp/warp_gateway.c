/* In-title gateway: Roblox's web traffic through a Cloudflare WARP tunnel,
 * so the PS5 needs no PC to reach Roblox (Qatar blocks it by DNS and by IP).
 *
 * The title's network layer (bionic_net.h) already sends every TCP
 * connection to a SOCKS5 gateway; this is that gateway, listening on the
 * PS5 itself. Behind it: lwIP (a small TCP/IP stack, one thread, no OS)
 * inside a WireGuard tunnel to Cloudflare WARP (wireguard-lwip). Encrypted
 * WireGuard packets leave through one ordinary UDP socket; names are looked
 * up by lwIP's DNS client at 1.1.1.1 inside the tunnel, so the local DNS
 * block does not apply.
 *
 * SOCKS5 as the title uses it: no authentication; CONNECT (1) by name or
 * IPv4; RESOLVE (0xF0, Tor's extension) answering the name's IPv4 address.
 *
 * Settings: a JSON file written by warp_register.c on first run (private_key,
 * peer_public_key, endpoint_ip, endpoint_port, address_v4, dns).
 *
 * Builds for the PS5 title and for the Linux host (warp_gateway_host.c), the
 * same code in both. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "wireguard-platform.h"
#include "wireguardif.h"

#include "warp_gateway.h"
#include "endpoint_config.h"
#include "endpoint_list.h"

/* wireguardif.c: a WireGuard packet received for the interface (not in its header) */
void wireguardif_network_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
#define WARP_TCP_NODELAY 1 /* netinet/tcp.h clashes with lwIP's TCP_MSS */

/* ---- platform pieces lwIP and WireGuard ask for ------------------------- */

static void (*warp_log_fn)(const char *line);
void
warp_log(const char *fmt, ...)
{
   char line[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   size_t n = strlen(line);
   while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
   if (warp_log_fn)
      warp_log_fn(line);
   else
      fprintf(stderr, "%s\n", line);
}

u32_t
sys_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (u32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

uint32_t wireguard_sys_now(void) { return sys_now(); }
bool wireguard_is_under_load(void) { return false; }

/* Kernel randomness from /dev/urandom (or /dev/random). The PS5 SDK exposes
 * no arc4random/getentropy to titles. Without either device, a BLAKE2s hash
 * of timer jitter is used and logged (the tunnel's static key comes from the
 * settings file either way; this only feeds session keys and nonces). */
#include "crypto/refc/blake2s.h"
void
wireguard_random_bytes(void *bytes, size_t size)
{
   static int fd = -2;
   if (fd == -2) {
      fd = open("/dev/urandom", O_RDONLY);
      if (fd < 0)
         fd = open("/dev/random", O_RDONLY);
      if (fd < 0)
         warp_log("warp: no /dev/urandom or /dev/random: using timer jitter for randomness");
   }
   uint8_t *b = bytes;
   while (size && fd >= 0) {
      const ssize_t n = read(fd, b, size);
      if (n <= 0)
         break;
      b += n;
      size -= (size_t)n;
   }
   static uint8_t pool[32];
   while (size) {
      blake2s_ctx ctx;
      blake2s_init(&ctx, 32, NULL, 0);
      blake2s_update(&ctx, pool, sizeof(pool));
      for (int i = 0; i < 64; i++) {
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         const uint64_t t = __builtin_ia32_rdtsc() ^ (uint64_t)ts.tv_nsec;
         blake2s_update(&ctx, &t, sizeof(t));
      }
      blake2s_final(&ctx, pool);
      const size_t n = size < 16 ? size : 16;
      memcpy(b, pool, n); /* hand out half, keep the rest of the state secret */
      b += n;
      size -= n;
   }
}

unsigned
warp_random32(void)
{
   unsigned r;
   wireguard_random_bytes(&r, sizeof(r));
   return r;
}

/* TAI64N: seconds since 1970 + 2^62 + 10 (big-endian 8 bytes), then
 * nanoseconds (big-endian 4 bytes). */
void
wireguard_tai64n_now(uint8_t *out)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   const uint64_t s = 0x400000000000000aull + (uint64_t)ts.tv_sec;
   const uint32_t ns = (uint32_t)ts.tv_nsec;
   for (int i = 0; i < 8; i++)
      out[i] = (uint8_t)(s >> (56 - 8 * i));
   for (int i = 0; i < 4; i++)
      out[8 + i] = (uint8_t)(ns >> (24 - 8 * i));
}

/* ---- the uplink: WireGuard's encrypted packets on a host UDP socket ----- */

static int up_fd = -1;
static struct netif wg_netif;
static uint64_t up_tx, up_rx, up_tx_pkts, up_rx_pkts, up_tx_err;
static int up_last_errno;

err_t
wg_uplink_sendto(struct pbuf *p, const ip_addr_t *ip, u16_t port)
{
   static uint8_t buf[2048];
   const u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
   struct sockaddr_in to;
   memset(&to, 0, sizeof(to));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
   to.sin_len = sizeof(to);
#endif
   to.sin_family = AF_INET;
   to.sin_port = htons(port);
   to.sin_addr.s_addr = ip_2_ip4(ip)->addr;
   if (sendto(up_fd, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)n) {
      up_tx_err++;
      up_last_errno = errno;
      return ERR_IF;
   }
   up_tx += n;
   up_tx_pkts++;
   return ERR_OK;
}

/* A reader thread empties the UDP socket straight into a large queue, so
 * a burst never overflows the kernel's buffer (the PS5 grants only about
 * 41 KiB; on the PC an undersized buffer dropped packets and cut throughput
 * six-fold). The lwIP thread takes packets from the queue; a pipe wakes it. */
#define RX_SLOTS 8192
#define RX_MAX 1600
static struct rx_slot {
   uint16_t len, port;
   uint32_t addr;
   uint8_t data[RX_MAX];
} *rx_ring;
static unsigned rx_head, rx_tail; /* head: written by the reader; tail: by lwIP */
static uint64_t rx_dropped;
static int rx_wake[2] = {-1, -1};

static void *
uplink_reader(void *arg)
{
   (void)arg;
   for (;;) {
      struct pollfd pf = {up_fd, POLLIN, 0};
      if (poll(&pf, 1, 1000) <= 0)
         continue;
      int got = 0;
      for (;;) {
         const unsigned head = __atomic_load_n(&rx_head, __ATOMIC_RELAXED);
         const unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE);
         struct rx_slot *slot = &rx_ring[head % RX_SLOTS];
         struct sockaddr_in from;
         socklen_t fl = sizeof(from);
         uint8_t spill[RX_MAX];
         const int full = head - tail >= RX_SLOTS;
         const ssize_t n = recvfrom(up_fd, full ? spill : slot->data, RX_MAX, MSG_DONTWAIT,
                                    (struct sockaddr *)&from, &fl);
         if (n <= 0)
            break;
         if (full) {
            rx_dropped++;
            continue;
         }
         slot->len = (uint16_t)n;
         slot->port = ntohs(from.sin_port);
         slot->addr = from.sin_addr.s_addr;
         __atomic_store_n(&rx_head, head + 1, __ATOMIC_RELEASE);
         got = 1;
      }
      if (got)
         (void)!write(rx_wake[1], "", 1);
   }
   return NULL;
}

/* packets received from Cloudflare so far (the title presses Retry on
 * Roblox's connection-error page only while this is moving) */
uint64_t
warp_rx_packets(void)
{
   return __atomic_load_n(&up_rx_pkts, __ATOMIC_RELAXED);
}

static void
uplink_receive(void)
{
   /* one read per wake-up: poll said the pipe is readable, so it cannot
    * block (a read-until-empty loop blocked for ever on the PS5, where the
    * non-blocking switch reaches sockets but not pipes) */
   uint8_t drain[256];
   (void)!read(rx_wake[0], drain, sizeof(drain));
   for (;;) {
      const unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_RELAXED);
      if (tail == __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE))
         return;
      struct rx_slot *slot = &rx_ring[tail % RX_SLOTS];
      /* test switch: while the file /data/roblox/files/warp-block exists,
       * everything Cloudflare sends is dropped (a stalled tunnel on demand) */
      {
         static u32_t checked;
         static int blocked;
         const u32_t now = sys_now();
         if (now - checked > 1000) {
            checked = now;
            const int b = access("/data/roblox/files/warp-block", F_OK) == 0;
            if (b != blocked)
               warp_log("warp: test block %s", b ? "on: dropping everything received" : "off");
            blocked = b;
         }
         if (blocked) {
            __atomic_store_n(&rx_tail, tail + 1, __ATOMIC_RELEASE);
            continue;
         }
      }
      up_rx += slot->len;
      __atomic_add_fetch(&up_rx_pkts, 1, __ATOMIC_RELAXED);
      struct pbuf *p = pbuf_alloc(PBUF_RAW, slot->len, PBUF_RAM);
      if (p) {
         memcpy(p->payload, slot->data, slot->len);
         ip_addr_t addr;
         IP_ADDR4(&addr, 0, 0, 0, 0);
         ip_2_ip4(&addr)->addr = slot->addr;
         wireguardif_network_rx(wg_netif.state, NULL, p, &addr, slot->port);
      }
      __atomic_store_n(&rx_tail, tail + 1, __ATOMIC_RELEASE);
   }
}

/* ---- SOCKS5 connections bridged to lwIP TCP ----------------------------- */

enum { C_FREE, C_GREET, C_REQUEST, C_WAIT_TUNNEL, C_RESOLVING, C_CONNECTING, C_OPEN, C_UDP };
#define MAX_CONNS 192
struct conn {
   int state;
   int fd;
   struct tcp_pcb *pcb;
   uint8_t in[512];
   size_t in_len;
   int resolve_only;
   char host[256];
   u16_t port;
   struct pbuf *pending; /* from the tunnel, not yet written to fd */
   int remote_closed;    /* the server finished sending */
   int local_eof;        /* the title finished sending */
   u32_t since;          /* C_WAIT_TUNNEL: when it started waiting */
   int dns_tries;        /* name lookups that timed out so far */
   int udp;              /* C_UDP: its association, index + 1 */
};
static struct conn conns[MAX_CONNS];
static unsigned n_opened, n_failed;

static void
set_nonblock(int fd)
{
#if defined(__PROSPERO__)
   int on = 1; /* SO_NBIO: the PS5's way (fcntl does not reach sockets) */
   setsockopt(fd, SOL_SOCKET, 0x1200, &on, sizeof(on));
#else
   const int fl = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void udp_assoc_free(int index);
static void
conn_close(struct conn *c)
{
   if (c->udp)
      udp_assoc_free(c->udp - 1);
   if (c->pcb) {
      tcp_arg(c->pcb, NULL);
      tcp_recv(c->pcb, NULL);
      tcp_sent(c->pcb, NULL);
      tcp_err(c->pcb, NULL);
      if (tcp_close(c->pcb) != ERR_OK)
         tcp_abort(c->pcb);
      c->pcb = NULL;
   }
   if (c->pending) {
      pbuf_free(c->pending);
      c->pending = NULL;
   }
   if (c->fd >= 0)
      close(c->fd);
   memset(c, 0, sizeof(*c));
   c->fd = -1;
   c->state = C_FREE;
}

static void
socks_reply_port(struct conn *c, uint8_t status, uint32_t addr /* network order */, uint16_t port /* host order */)
{
   uint8_t r[10] = {5, status, 0, 1};
   memcpy(r + 4, &addr, 4);
   r[8] = (uint8_t)(port >> 8), r[9] = (uint8_t)port;
   (void)!write(c->fd, r, sizeof(r));
}
static void
socks_reply(struct conn *c, uint8_t status, uint32_t addr /* network order */)
{
   socks_reply_port(c, status, addr, 0);
}

static err_t
on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
   (void)pcb;
   (void)err;
   struct conn *c = arg;
   if (!c)
      return ERR_OK;
   if (!p) {
      c->remote_closed = 1;
      return ERR_OK;
   }
   if (c->pending)
      pbuf_cat(c->pending, p);
   else
      c->pending = p;
   return ERR_OK;
}

static void
on_err(void *arg, err_t err)
{
   struct conn *c = arg;
   if (!c)
      return;
   c->pcb = NULL; /* lwIP has freed it */
   if (c->state == C_CONNECTING) {
      n_failed++;
      warp_log("warp: connect %s:%u failed (%d)", c->host, c->port, err);
      socks_reply(c, 5, 0);
      conn_close(c);
   } else {
      c->remote_closed = 1;
   }
}

static err_t
on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
   struct conn *c = arg;
   if (!c)
      return ERR_OK;
   if (err != ERR_OK) {
      socks_reply(c, 5, 0);
      conn_close(c);
      return ERR_OK;
   }
   c->state = C_OPEN;
   n_opened++;
   socks_reply(c, 0, ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip)));
   return ERR_OK;
}

static void
start_connect(struct conn *c, const ip_addr_t *ip)
{
   if (c->resolve_only) {
      socks_reply(c, 0, ip4_addr_get_u32(ip_2_ip4(ip)));
      conn_close(c);
      return;
   }
   c->pcb = tcp_new();
   if (!c->pcb) {
      socks_reply(c, 1, 0);
      conn_close(c);
      return;
   }
   tcp_nagle_disable(c->pcb);
   tcp_arg(c->pcb, c);
   tcp_recv(c->pcb, on_recv);
   tcp_err(c->pcb, on_err);
   c->state = C_CONNECTING;
   if (tcp_connect(c->pcb, ip, c->port, on_connected) != ERR_OK) {
      socks_reply(c, 1, 0);
      conn_close(c);
   }
}

static void
on_dns(const char *name, const ip_addr_t *ip, void *arg)
{
   (void)name;
   struct conn *c = arg;
   if (c->state != C_RESOLVING)
      return;
   if (!ip) {
      /* a lookup lost in the tunnel: queue it for another try (the loop
       * starts it again once lwIP has dropped the failed entry) */
      if (++c->dns_tries < 3) {
         c->state = C_WAIT_TUNNEL;
         c->since = sys_now();
         return;
      }
      n_failed++;
      warp_log("warp: no address for %s", c->host);
      socks_reply(c, 4, 0);
      conn_close(c);
      return;
   }
   start_connect(c, ip);
}

static u8_t peer_index = WIREGUARDIF_INVALID_INDEX;
static int
tunnel_up(void)
{
   ip_addr_t ip;
   u16_t port;
   return peer_index != WIREGUARDIF_INVALID_INDEX &&
          wireguardif_peer_is_up(&wg_netif, peer_index, &ip, &port) == ERR_OK;
}

/* Name lookup and connect, once the tunnel is up. */
static void
socks_go(struct conn *c)
{
   ip_addr_t ip;
   if (ipaddr_aton(c->host, &ip)) {
      start_connect(c, &ip);
      return;
   }
   c->state = C_RESOLVING;
   const err_t e = dns_gethostbyname(c->host, &ip, on_dns, c);
   if (e == ERR_OK)
      start_connect(c, &ip);
   else if (e != ERR_INPROGRESS) {
      n_failed++;
      socks_reply(c, 4, 0);
      conn_close(c);
   }
}

/* ---- SOCKS5 UDP ASSOCIATE: the game's own traffic ---------------------
 * Roblox's game servers speak UDP. It used to go straight out of the PS5,
 * and some networks (Qatar, Nov 25) stopped answering it: every join ended
 * in "no response from server" (error 279). An association is a relay
 * socket on 127.0.0.1 and an lwIP UDP socket inside the tunnel; datagrams
 * carry the SOCKS5 UDP header (RFC 1928 section 7) on the relay side. It
 * lives as long as its SOCKS control connection. */
#define MAX_UDP 64
struct uassoc {
   int used;
   int fd;              /* relay socket, 127.0.0.1 */
   struct udp_pcb *pcb; /* inside the tunnel */
   struct sockaddr_in client;
   int have_client;
};
static struct uassoc uas[MAX_UDP];
static unsigned long long n_udp_out, n_udp_in, n_udp_drop, n_udp_assoc;
static uint8_t udp_buf[10 + 65536];

static void
udp_assoc_free(int i)
{
   struct uassoc *u = &uas[i];
   if (!u->used)
      return;
   if (u->pcb)
      udp_remove(u->pcb);
   if (u->fd >= 0)
      close(u->fd);
   memset(u, 0, sizeof(*u));
   u->fd = -1;
}

/* tunnel -> title */
static void
udp_from_tunnel(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
   (void)pcb;
   struct uassoc *u = arg;
   if (!u || !u->used || !u->have_client) { /* tot_len (u16) always fits udp_buf */
      n_udp_drop++;
      pbuf_free(p);
      return;
   }
   const uint32_t a = ip4_addr_get_u32(ip_2_ip4(addr));
   udp_buf[0] = udp_buf[1] = udp_buf[2] = 0;
   udp_buf[3] = 1;
   memcpy(udp_buf + 4, &a, 4);
   udp_buf[8] = (uint8_t)(port >> 8), udp_buf[9] = (uint8_t)port;
   const u16_t n = pbuf_copy_partial(p, udp_buf + 10, p->tot_len, 0);
   pbuf_free(p);
   if (sendto(u->fd, udp_buf, 10u + n, 0, (struct sockaddr *)&u->client, sizeof(u->client)) < 0)
      n_udp_drop++;
   else
      n_udp_in++;
}

/* title -> tunnel: everything waiting on the relay socket */
static int tunnel_up(void);
static void
udp_from_title(struct uassoc *u)
{
   for (int k = 0; k < 256; k++) {
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      const ssize_t r = recvfrom(u->fd, udp_buf, sizeof(udp_buf), 0, (struct sockaddr *)&from, &fl);
      if (r < 0)
         return;
      /* only the title on this console, and only plain IPv4 datagrams */
      if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || r < 10 || udp_buf[2] != 0 || udp_buf[3] != 1) {
         n_udp_drop++;
         continue;
      }
      u->client = from;
      u->have_client = 1;
      if (!tunnel_up()) {
         n_udp_drop++;
         continue;
      }
      ip_addr_t ip;
      uint32_t a;
      memcpy(&a, udp_buf + 4, 4);
      ip_addr_set_ip4_u32(&ip, a);
      const u16_t port = (u16_t)((udp_buf[8] << 8) | udp_buf[9]);
      struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(r - 10), PBUF_RAM);
      if (!p) {
         n_udp_drop++;
         continue;
      }
      memcpy(p->payload, udp_buf + 10, (size_t)(r - 10));
      if (udp_sendto(u->pcb, p, &ip, port) == ERR_OK)
         n_udp_out++;
      else
         n_udp_drop++;
      pbuf_free(p);
   }
}

/* A UDP ASSOCIATE request: the relay's port in the reply. */
static void
udp_associate(struct conn *c)
{
   int i = 0;
   while (i < MAX_UDP && uas[i].used)
      i++;
   struct uassoc *u = i < MAX_UDP ? &uas[i] : NULL;
   if (u) {
      memset(u, 0, sizeof(*u));
      u->used = 1;
      u->fd = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in la;
      memset(&la, 0, sizeof(la));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
      la.sin_len = sizeof(la);
#endif
      la.sin_family = AF_INET;
      la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      socklen_t ll = sizeof(la);
      u->pcb = udp_new();
      if (u->fd < 0 || bind(u->fd, (struct sockaddr *)&la, sizeof(la)) != 0 ||
          getsockname(u->fd, (struct sockaddr *)&la, &ll) != 0 || !u->pcb ||
          udp_bind(u->pcb, IP_ADDR_ANY, 0) != ERR_OK) {
         warp_log("warp: UDP association failed (errno %d)", errno);
         udp_assoc_free(i);
         u = NULL;
      } else {
         int bufsz = 1 << 20;
         setsockopt(u->fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
         setsockopt(u->fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
         set_nonblock(u->fd);
         udp_recv(u->pcb, udp_from_tunnel, u);
         c->udp = i + 1;
         c->state = C_UDP;
         n_udp_assoc++;
         socks_reply_port(c, 0, htonl(INADDR_LOOPBACK), ntohs(la.sin_port));
         return;
      }
   }
   socks_reply(c, 1, 0);
   conn_close(c);
}

/* A SOCKS greeting or request has arrived in c->in. */
static void
socks_step(struct conn *c)
{
   if (c->state == C_GREET) {
      if (c->in_len < 2 || c->in_len < 2u + c->in[1])
         return;
      const size_t used = 2u + c->in[1];
      memmove(c->in, c->in + used, c->in_len - used);
      c->in_len -= used;
      (void)!write(c->fd, (uint8_t[]){5, 0}, 2);
      c->state = C_REQUEST;
   }
   if (c->state != C_REQUEST || c->in_len < 5)
      return;
   const uint8_t cmd = c->in[1], atyp = c->in[3];
   size_t need;
   if (atyp == 1)
      need = 4 + 4 + 2;
   else if (atyp == 3)
      need = 4 + 1 + c->in[4] + 2;
   else {
      socks_reply(c, 8, 0);
      conn_close(c);
      return;
   }
   if (c->in_len < need)
      return;
   if (cmd == 3) {
      c->in_len = 0;
      udp_associate(c);
      return;
   }
   c->resolve_only = cmd == 0xF0;
   if (cmd != 1 && cmd != 0xF0) {
      socks_reply(c, 7, 0);
      conn_close(c);
      return;
   }
   c->port = (u16_t)((c->in[need - 2] << 8) | c->in[need - 1]);
   if (atyp == 1)
      snprintf(c->host, sizeof(c->host), "%u.%u.%u.%u", c->in[4], c->in[5], c->in[6], c->in[7]);
   else
      snprintf(c->host, sizeof(c->host), "%.*s", c->in[4], (const char *)c->in + 5);
   c->in_len = 0;
   /* Until the tunnel is up, hold the request: failing it made the app's
    * start-up sign-in check fail and show the signed-out landing page. */
   if (!tunnel_up()) {
      c->state = C_WAIT_TUNNEL;
      c->since = sys_now();
      return;
   }
   socks_go(c);
}

/* Tunnel -> title: write what fits, tell lwIP to reopen its window. */
static void
flush_pending(struct conn *c)
{
   while (c->pending) {
      struct pbuf *p = c->pending;
      const ssize_t w = write(c->fd, p->payload, p->len);
      if (w <= 0) {
         if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            conn_close(c);
         }
         return;
      }
      if (c->pcb)
         tcp_recved(c->pcb, (u16_t)w);
      if ((size_t)w < p->len) {
         pbuf_remove_header(p, (size_t)w);
         return;
      }
      c->pending = p->next;
      if (c->pending)
         pbuf_ref(c->pending);
      pbuf_free(p);
   }
}

/* Title -> tunnel: read only as much as lwIP can queue. */
static void
pump_local(struct conn *c)
{
   uint8_t buf[16384];
   if (!c->pcb)
      return;
   const u16_t room = tcp_sndbuf(c->pcb);
   if (!room || tcp_sndqueuelen(c->pcb) > TCP_SND_QUEUELEN - 8)
      return;
   const ssize_t n = read(c->fd, buf, room < sizeof(buf) ? room : sizeof(buf));
   if (n == 0) {
      c->local_eof = 1;
      tcp_shutdown(c->pcb, 0, 1);
      return;
   }
   if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
         conn_close(c);
      return;
   }
   if (tcp_write(c->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
      conn_close(c);
      return;
   }
   tcp_output(c->pcb);
}

/* ---- the loop ----------------------------------------------------------- */

struct warp_conf {
   char private_key[64], peer_public_key[64], endpoint_ip[64], address_v4[32], dns[32];
   unsigned endpoint_port;
};

static int
json_str(const char *j, const char *key, char *out, size_t n)
{
   char pat[64];
   snprintf(pat, sizeof(pat), "\"%s\"", key);
   const char *p = strstr(j, pat);
   if (!p)
      return -1;
   p = strchr(p + strlen(pat), ':');
   if (!p)
      return -1;
   p++;
   while (*p == ' ')
      p++;
   if (*p == '"') {
      const char *e = strchr(++p, '"');
      if (!e || (size_t)(e - p) >= n)
         return -1;
      memcpy(out, p, (size_t)(e - p));
      out[e - p] = 0;
   } else {
      size_t i = 0;
      while (p[i] && p[i] != ',' && p[i] != '}' && i + 1 < n) {
         out[i] = p[i];
         i++;
      }
      if (p[i] && p[i] != ',' && p[i] != '}')
         return -1;
      while (i && (out[i - 1] == ' ' || out[i - 1] == '\t' ||
                   out[i - 1] == '\r' || out[i - 1] == '\n'))
         i--;
      out[i] = 0;
   }
   return 0;
}

static int
load_conf(const char *path, struct warp_conf *c)
{
   char j[2048] = {0};
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   (void)!fread(j, 1, sizeof(j) - 1, f);
   fclose(f);
   char port[16];
   unsigned short parsed_port;
   if (json_str(j, "private_key", c->private_key, sizeof(c->private_key)) ||
       json_str(j, "peer_public_key", c->peer_public_key, sizeof(c->peer_public_key)) ||
       json_str(j, "endpoint_ip", c->endpoint_ip, sizeof(c->endpoint_ip)) ||
       json_str(j, "address_v4", c->address_v4, sizeof(c->address_v4)))
      return -1;
   if (json_str(j, "endpoint_port", port, sizeof(port)) || warp_endpoint_port(port, &parsed_port))
      return -1;
   if (json_str(j, "dns", c->dns, sizeof(c->dns)))
      snprintf(c->dns, sizeof(c->dns), "1.1.1.1");
   c->endpoint_port = parsed_port;
   return 0;
}

static struct warp_conf conf;
static int listen_fd = -1;
static char saved_endpoint_path[512]; /* <conf dir>/warp-endpoint.txt */
static int
setup(const char *conf_path, unsigned short port)
{
   {
      const char *slash = strrchr(conf_path, '/');
      snprintf(saved_endpoint_path, sizeof(saved_endpoint_path), "%.*s/warp-endpoint.txt",
               slash ? (int)(slash - conf_path) : 1, slash ? conf_path : ".");
   }
   if (load_conf(conf_path, &conf) != 0) {
      warp_log("warp: cannot read %s", conf_path);
      return -1;
   }
   lwip_init();

   up_fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (up_fd < 0)
      return -1;
   /* A fixed source port: after a relaunch the tunnel continues from the same
    * address and port, which Cloudflare answers at once; from a new port it
    * ignored handshakes for a while (host test: 6 tries, no reply). */
   {
      /* The PS5 refused 51821 with EACCES (13) on every run, so each relaunch
       * came from a new port and Cloudflare could stay silent for minutes
       * (the app then showed its signed-out page). Try a few ports, without
       * SO_REUSEADDR first, and log why each one is refused. */
      const char *lp = getenv("WARP_LOCAL_PORT");
      const uint16_t ports[] = {(uint16_t)(lp ? atoi(lp) : 3478), 3478, 51821, 50001, 60001}; /* 3478 is the one the PS5 allows */
      int bound = 0;
      for (unsigned k = 0; k < sizeof(ports) / sizeof(ports[0]) && !bound; k++) {
         for (int reuse = 0; reuse < 2 && !bound; reuse++) {
            if (reuse) {
               int one = 1;
               if (setsockopt(up_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
                  warp_log("warp: SO_REUSEADDR refused (%d)", errno);
            }
            struct sockaddr_in me;
            memset(&me, 0, sizeof(me));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
            me.sin_len = sizeof(me);
#endif
            me.sin_family = AF_INET;
            me.sin_port = htons(ports[k]);
            if (bind(up_fd, (struct sockaddr *)&me, sizeof(me)) == 0) {
               bound = 1;
               warp_log("warp: source port %u%s", ports[k], reuse ? " (reuse)" : "");
            } else {
               warp_log("warp: source port %u refused (%d)%s", ports[k], errno, reuse ? " (reuse)" : "");
            }
         }
      }
      if (!bound)
         warp_log("warp: no fixed source port; using any");
   }
   set_nonblock(up_fd);
   /* A full receive window arrives as a burst of WireGuard packets; with the
    * default socket buffer they were dropped (and TCP slowed to recover). */
   const char *want = getenv("WARP_UDP_RCVBUF"); /* testing: e.g. the PS5's 41984 on the PC */
   int bufsz = want ? atoi(want) : 8 << 20;
   setsockopt(up_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
   setsockopt(up_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
   socklen_t bl = sizeof(bufsz);
   getsockopt(up_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, &bl);
   warp_log("warp: uplink receive buffer %d KiB", bufsz >> 10);
   rx_ring = calloc(RX_SLOTS, sizeof(*rx_ring));
   if (!rx_ring || pipe(rx_wake) != 0)
      return -1;
   pthread_t rt;
   if (pthread_create(&rt, NULL, uplink_reader, NULL) != 0)
      return -1;
   pthread_detach(rt);

   static struct wireguardif_init_data wg;
   wg.private_key = conf.private_key;
   wg.listen_port = 51820;
   wg.bind_netif = NULL;
   ip4_addr_t addr, mask, gw;
   ip4addr_aton(conf.address_v4, &addr);
   IP4_ADDR(&mask, 255, 255, 255, 255);
   IP4_ADDR(&gw, 0, 0, 0, 0);
   if (!netif_add(&wg_netif, &addr, &mask, &gw, &wg, wireguardif_init, ip4_input)) {
      warp_log("warp: WireGuard interface failed");
      return -1;
   }
   wg_netif.mtu = 1280;
   netif_set_default(&wg_netif);
   netif_set_up(&wg_netif);
   netif_set_link_up(&wg_netif);

   struct wireguardif_peer peer;
   wireguardif_peer_init(&peer);
   peer.public_key = conf.peer_public_key;
   peer.preshared_key = NULL;
   IP_ADDR4(&peer.allowed_ip, 0, 0, 0, 0);
   IP_ADDR4(&peer.allowed_mask, 0, 0, 0, 0);
   /* Start with the endpoint supplied by registration rather than one measured
    * in a particular country. An explicit WARP_ENDPOINT=ip:port can override it. */
   {
      char selected[sizeof(conf.endpoint_ip)];
      unsigned short port;
      if (warp_endpoint_select(conf.endpoint_ip, conf.endpoint_port, getenv("WARP_ENDPOINT"),
                               selected, sizeof(selected), &port) != 0) {
         warp_log("warp: invalid registration endpoint or WARP_ENDPOINT override");
         return -1;
      }
      snprintf(conf.endpoint_ip, sizeof(conf.endpoint_ip), "%s", selected);
      conf.endpoint_port = port;
      /* Without an explicit override, start with the endpoint that last
       * brought the tunnel up on this console (saved by tunnel_watch). */
      char saved[sizeof(conf.endpoint_ip)];
      unsigned short saved_port;
      if (!getenv("WARP_ENDPOINT") && warp_endpoint_load(saved_endpoint_path, saved, sizeof(saved), &saved_port) == 0) {
         snprintf(conf.endpoint_ip, sizeof(conf.endpoint_ip), "%s", saved);
         conf.endpoint_port = saved_port;
         warp_log("warp: starting with the endpoint that worked last time: %s:%u", saved, saved_port);
      }
   }
   ipaddr_aton(conf.endpoint_ip, &peer.endpoint_ip);
   peer.endport_port = (u16_t)conf.endpoint_port;
   peer.keep_alive = 25;
   if (wireguardif_add_peer(&wg_netif, &peer, &peer_index) != ERR_OK) {
      warp_log("warp: adding the WARP peer failed");
      return -1;
   }
   wireguardif_connect(&wg_netif, peer_index);

   ip_addr_t dns1, dns2;
   ipaddr_aton(conf.dns, &dns1);
   IP_ADDR4(&dns2, 1, 0, 0, 1);
   dns_setserver(0, &dns1);
   dns_setserver(1, &dns2);

   listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   int one = 1;
   setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in la;
   memset(&la, 0, sizeof(la));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
   la.sin_len = sizeof(la);
#endif
   la.sin_family = AF_INET;
   la.sin_port = htons(port);
   la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(listen_fd, (struct sockaddr *)&la, sizeof(la)) != 0 || listen(listen_fd, 64) != 0) {
      warp_log("warp: cannot listen on 127.0.0.1:%u (%d)", port, errno);
      return -1;
   }
   set_nonblock(listen_fd);
   for (int i = 0; i < MAX_CONNS; i++)
      conns[i].fd = -1;
   for (int i = 0; i < MAX_UDP; i++)
      uas[i].fd = -1;
   warp_log("warp: gateway on 127.0.0.1:%u, tunnel %s -> %s:%u", port, conf.address_v4, conf.endpoint_ip,
            conf.endpoint_port);
   return 0;
}

static void
loop_once(void)
{
   struct pollfd pf[MAX_CONNS + MAX_UDP + 2];
   int map[MAX_CONNS + MAX_UDP + 2];
   int n = 0;
   pf[n].fd = rx_wake[0], pf[n].events = POLLIN, map[n++] = -1;
   pf[n].fd = listen_fd, pf[n].events = POLLIN, map[n++] = -2;
   for (int i = 0; i < MAX_CONNS; i++) {
      struct conn *c = &conns[i];
      if (c->state == C_FREE)
         continue;
      short ev = 0;
      if (c->state == C_GREET || c->state == C_REQUEST || c->state == C_UDP)
         ev |= POLLIN;
      if (c->state == C_OPEN && !c->local_eof && c->pcb && tcp_sndbuf(c->pcb) > 0)
         ev |= POLLIN;
      if (c->pending)
         ev |= POLLOUT;
      pf[n].fd = c->fd, pf[n].events = ev, map[n++] = i;
   }
   for (int i = 0; i < MAX_UDP; i++)
      if (uas[i].used)
         pf[n].fd = uas[i].fd, pf[n].events = POLLIN, map[n++] = -3 - i;
   u32_t wait = sys_timeouts_sleeptime();
   if (wait > 20)
      wait = 20;
   poll(pf, (nfds_t)n, (int)wait);

   for (int k = 0; k < n; k++) {
      if (!pf[k].revents)
         continue;
      if (map[k] <= -3) {
         if (uas[-3 - map[k]].used)
            udp_from_title(&uas[-3 - map[k]]);
      } else if (map[k] == -1) {
         uplink_receive();
      } else if (map[k] == -2) {
         for (;;) {
            const int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0)
               break;
            int slot = -1;
            for (int i = 0; i < MAX_CONNS && slot < 0; i++)
               if (conns[i].state == C_FREE)
                  slot = i;
            if (slot < 0) {
               close(fd);
               continue;
            }
            set_nonblock(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, WARP_TCP_NODELAY, &one, sizeof(one));
            memset(&conns[slot], 0, sizeof(conns[slot]));
            conns[slot].fd = fd;
            conns[slot].state = C_GREET;
         }
      } else {
         struct conn *c = &conns[map[k]];
         if (c->state == C_FREE)
            continue;
         if (pf[k].revents & POLLOUT)
            flush_pending(c);
         if (c->state == C_FREE)
            continue;
         if (pf[k].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (c->state == C_GREET || c->state == C_REQUEST) {
               const ssize_t r = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len);
               if (r <= 0) {
                  conn_close(c);
                  continue;
               }
               c->in_len += (size_t)r;
               socks_step(c);
            } else if (c->state == C_UDP) {
               /* the control connection: closing it ends the association */
               uint8_t junk[64];
               const ssize_t r = read(c->fd, junk, sizeof(junk));
               if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                  conn_close(c);
            } else if (c->state == C_OPEN) {
               pump_local(c);
            } else if (pf[k].revents & (POLLHUP | POLLERR)) {
               conn_close(c);
            }
         }
      }
   }
   sys_check_timeouts();
   /* deliver what arrived during the timers, and finish closed ones */
   const int up = tunnel_up();
   for (int i = 0; i < MAX_CONNS; i++) {
      struct conn *c = &conns[i];
      if (c->state == C_FREE)
         continue;
      if (c->state == C_WAIT_TUNNEL) {
         if (up)
            socks_go(c);
         else if (sys_now() - c->since > 25000) {
            n_failed++;
            socks_reply(c, 1, 0);
            conn_close(c);
         }
         continue;
      }
      if (c->pending)
         flush_pending(c);
      if (c->state != C_FREE && c->remote_closed && !c->pending)
         conn_close(c);
   }
}

/* Endpoint rotation. Cloudflare sometimes stops answering one endpoint (the
 * PS5 saw 4 packets back and then silence, Nov 14), and some countries/ISPs
 * block WireGuard on particular ports or address ranges. Slot 0 is the start
 * endpoint (WARP_ENDPOINT, else the saved one, else the registration's); after
 * it come the candidates of endpoint_list.h, which vary range and port on
 * every step. Rules (checked on the tunnel thread):
 *  - never up yet: move on after WARP_FIRST_MS (default 6 s) per endpoint;
 *  - was up: move on when at least 8 packets went out and at most one came
 *    back in a WARP_WATCH_MS window (default 10 s), or when the title asks
 *    (warp_rotate_endpoint: a slow but not silent tunnel).
 * The endpoint that brings the tunnel up is saved for the next start. */
static volatile int rotate_request;
void
warp_rotate_endpoint(void)
{
   __atomic_store_n(&rotate_request, 1, __ATOMIC_RELEASE);
}

static unsigned endpoint_at; /* 0 = start endpoint, k = candidate k-1 */
static char cur_ip[sizeof(conf.endpoint_ip)];
static u16_t cur_port;
static int was_up, saved_this;
static uint64_t watch_rx, watch_tx;
static u32_t watch_since, endpoint_since;

static void
use_endpoint(unsigned at)
{
   endpoint_at = at;
   if (at == 0) {
      snprintf(cur_ip, sizeof(cur_ip), "%s", conf.endpoint_ip);
      cur_port = (u16_t)conf.endpoint_port;
   } else {
      unsigned short p = 0;
      warp_endpoint_candidate(at - 1, cur_ip, sizeof(cur_ip), &p);
      cur_port = p;
   }
   ip_addr_t ip;
   ipaddr_aton(cur_ip, &ip);
   wireguardif_disconnect(&wg_netif, peer_index);
   wireguardif_update_endpoint(&wg_netif, peer_index, &ip, cur_port);
   wireguardif_connect(&wg_netif, peer_index);
   endpoint_since = sys_now();
}

static void
tunnel_watch(void)
{
   const u32_t now = sys_now();
   static u32_t period, first;
   if (!period) {
      const char *e = getenv("WARP_WATCH_MS");
      period = e && atoi(e) >= 3000 ? (u32_t)atoi(e) : 10000;
      e = getenv("WARP_FIRST_MS");
      first = e && atoi(e) >= 2000 ? (u32_t)atoi(e) : 6000;
      snprintf(cur_ip, sizeof(cur_ip), "%s", conf.endpoint_ip);
      cur_port = (u16_t)conf.endpoint_port;
      endpoint_since = now;
   }
   const int up = tunnel_up();
   if (up && !saved_this) {
      saved_this = 1;
      warp_log("warp: tunnel up via %s:%u after %u ms on it", cur_ip, cur_port, (unsigned)(now - endpoint_since));
      if (warp_endpoint_save(saved_endpoint_path, cur_ip, cur_port) != 0)
         warp_log("warp: could not save the working endpoint");
   }
   if (up)
      was_up = 1;
   const unsigned total = warp_endpoint_candidate_count() + 1;
   if (!was_up) {
      if (now - endpoint_since < first)
         return;
      const unsigned next = (endpoint_at + 1) % total;
      use_endpoint(next);
      warp_log("warp: no answer yet: trying endpoint %s:%u (%u of %u)", cur_ip, cur_port, next + 1, total);
      return;
   }
   int forced = 0;
   if (__atomic_exchange_n(&rotate_request, 0, __ATOMIC_ACQ_REL))
      forced = 1;
   else if (now - watch_since < period)
      return;
   const uint64_t rx = __atomic_load_n(&up_rx_pkts, __ATOMIC_RELAXED);
   const uint64_t tx = up_tx_pkts;
   const int silent = forced || (watch_since && rx - watch_rx <= 1 && tx - watch_tx >= 8);
   watch_rx = rx;
   watch_tx = tx;
   watch_since = now;
   if (!silent)
      return;
   use_endpoint((endpoint_at + 1) % total);
   saved_this = 0; /* save the next one that works */
   warp_log(forced ? "warp: slow tunnel (app cannot reach its servers): trying endpoint %s:%u"
                   : "warp: no answer: trying endpoint %s:%u", cur_ip, cur_port);
}

static uint64_t last_status;
static void
status(void)
{
   const uint64_t now = sys_now();
   if (now - last_status < (getenv("WARP_STATUS_FAST") ? 3000u : 30000u))
      return;
   last_status = now;
   ip_addr_t ip;
   u16_t port;
   const int up = wireguardif_peer_is_up(&wg_netif, peer_index, &ip, &port) == ERR_OK;
   int active = 0;
   for (int i = 0; i < MAX_CONNS; i++)
      active += conns[i].state != C_FREE;
   warp_log("warp: tunnel %s, %d connections, %u opened, %u failed; %llu KiB in %llu packets out, %llu KiB in "
            "%llu packets in; %llu send errors (last errno %d), %llu queue drops; game UDP: %llu associations, "
            "%llu out, %llu in, %llu dropped",
            up ? "up" : "down", active, n_opened, n_failed, (unsigned long long)(up_tx >> 10),
            (unsigned long long)up_tx_pkts, (unsigned long long)(up_rx >> 10), (unsigned long long)up_rx_pkts,
            (unsigned long long)up_tx_err, up_last_errno, (unsigned long long)rx_dropped, n_udp_assoc, n_udp_out,
            n_udp_in, n_udp_drop);
}

int
warp_gateway_run(const char *conf_path, unsigned short port, void (*log)(const char *line))
{
   warp_log_fn = log;
   if (setup(conf_path, port) != 0)
      return -1;
   for (;;) {
      loop_once();
      tunnel_watch();
      status();
   }
   return 0;
}

static void *
warp_thread(void *arg)
{
   (void)arg;
   for (;;) {
      loop_once();
      tunnel_watch();
      status();
   }
   return NULL;
}

int
warp_gateway_start(const char *conf_path, unsigned short port, void (*log)(const char *line))
{
   /* set up here, so the gateway is listening before this returns */
   warp_log_fn = log;
   if (setup(conf_path, port) != 0)
      return -1;
   pthread_t t;
   if (pthread_create(&t, NULL, warp_thread, NULL) != 0)
      return -1;
   pthread_detach(t);
   return 0;
}
