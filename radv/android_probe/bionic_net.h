/* Networking for the engine: Linux sockets on the PS5, with every TCP
 * connection carried to the PC's gateway (SOCKS5, hostnames resolved there
 * over DNS-over-HTTPS and tunnelled through Cloudflare WARP, because Qatar
 * blocks Roblox by DNS and by IP). No PS5 or router network settings change.
 *
 * - getaddrinfo answers every name with a placeholder IPv4 address from
 *   198.18.0.0/16 and remembers the name; connect() to such an address opens
 *   a TCP connection to the gateway and asks it (SOCKS5, by name) for the
 *   real host. Numeric addresses are passed through by IP.
 * - Linux struct sockaddr_in {u16 family, u16 port, u32 addr} vs FreeBSD's
 *   {u8 len, u8 family, ...}; Linux socket levels/options, MSG_* flags and
 *   errno values differ and are translated.
 * - epoll does not exist on the PS5: an epoll descriptor is emulated on poll().
 *
 * Included by bionic_shims.h. ROBLOX_GATEWAY=ip:port overrides the gateway. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include "gateway_config.h"

static int bn_trace_on = -1;
static unsigned bn_trace_n;
#define BN_T(...)                                                                                       \
   do {                                                                                                \
      if (bn_trace_on < 0)                                                                             \
         bn_trace_on = getenv("ROBLOX_NETTRACE") != NULL;                                              \
      if (bn_trace_on && bn_trace_n++ < 600)                                                           \
         bionic_say("nettrace: " __VA_ARGS__);                                                         \
   } while (0)

/* ---- Linux errno for what the engine compares ---------------------------- */
static int
bn_linux_errno(int e)
{
   switch (e) {
   case EAGAIN: return 11;
   case EINPROGRESS: return 115;
   case EALREADY: return 114;
   case ECONNREFUSED: return 111;
   case ECONNRESET: return 104;
   case ECONNABORTED: return 103;
   case ETIMEDOUT: return 110;
   case ENOTCONN: return 107;
   case EISCONN: return 106;
   case ENETUNREACH: return 101;
   case EHOSTUNREACH: return 113;
   case EADDRINUSE: return 98;
   case EADDRNOTAVAIL: return 99;
   case EAFNOSUPPORT: return 97;
   case ENOTSOCK: return 88;
   case EMSGSIZE: return 90;
   case ENOPROTOOPT: return 92;
   case EPROTONOSUPPORT: return 93;
   case EOPNOTSUPP: return 95;
   case ENOBUFS: return 105;
   case EDESTADDRREQ: return 89;
   case ESHUTDOWN: return 108;
   default: return e; /* EINTR 4, EBADF 9, EINVAL 22, EPIPE 32, ENOMEM 12, EACCES 13, EFAULT 14 agree */
   }
}
#define BN_FAIL(rc)                                                                                   \
   do {                                                                                              \
      errno = bn_linux_errno(errno);                                                                 \
      return rc;                                                                                     \
   } while (0)

/* ---- placeholder addresses for names ------------------------------------- */
#define BN_NAMES 1024
static char bn_names[BN_NAMES][128];
static unsigned bn_name_count;
static pthread_mutex_t bn_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *
bn_name_of(uint32_t addr /* host order */)
{
   if ((addr & 0xFFFF0000u) != 0xC6120000u)
      return NULL;
   const unsigned i = (addr & 0xFFFF) - 1;
   return i < bn_name_count ? bn_names[i] : NULL;
}

/* ---- per-socket state ------------------------------------------------------ */
#define BN_FDS 4096
static struct {
   uint8_t used, tcp, via_gateway, nonblock; /* nonblock: emulated with MSG_DONTWAIT */
   uint32_t peer_addr; /* network order, as the engine connected to */
   uint16_t peer_port; /* network order */
} bn_fd[BN_FDS];


/* Non-blocking sockets. The PS5 refuses F_SETFL O_NONBLOCK and FIONBIO on
 * sockets (EACCES); Sony's own option is SO_NBIO (0x1200). If that fails too
 * the socket stays blocking and each send/receive gets MSG_DONTWAIT. */
#define BN_SO_NBIO 0x1200
static int bn_is_socket(int fd) { return fd >= 0 && fd < BN_FDS && bn_fd[fd].used; }
static int bn_is_nonblock(int fd) { return bn_is_socket(fd) && bn_fd[fd].nonblock; }
static int
bn_set_nonblock(int fd, int on)
{
   int v = on;
   const int r = setsockopt(fd, SOL_SOCKET, BN_SO_NBIO, &v, sizeof(v));
   static unsigned said;
   if (said++ < 3)
      bionic_say("net: socket %d non-blocking %d: SO_NBIO -> %d (errno %d)%s", fd, on, r, r ? errno : 0,
                 r ? "; emulated with MSG_DONTWAIT" : "");
   bn_fd[fd].nonblock = (uint8_t)on;
   return 0;
}
static int
bn_dontwait(int fd)
{
   return bn_is_nonblock(fd) ? MSG_DONTWAIT : 0;
}

/* Linux sockaddr (family u16 first) <-> FreeBSD (len u8, family u8) */
static int
bn_to_bsd(const void *lin, socklen_t len, struct sockaddr_storage *out, socklen_t *outlen)
{
   if (!lin || len < 2)
      return -1;
   uint16_t fam;
   memcpy(&fam, lin, 2);
   memset(out, 0, sizeof(*out));
   if (fam == 2 && len >= 8) {
      struct sockaddr_in *s = (struct sockaddr_in *)out;
      s->sin_len = sizeof(*s);
      s->sin_family = AF_INET;
      memcpy(&s->sin_port, (const char *)lin + 2, 2);
      memcpy(&s->sin_addr, (const char *)lin + 4, 4);
      *outlen = sizeof(*s);
      return 0;
   }
   if (fam == 10 && len >= 24) {
      struct sockaddr_in6 *s = (struct sockaddr_in6 *)out;
      s->sin6_len = sizeof(*s);
      s->sin6_family = AF_INET6;
      memcpy(&s->sin6_port, (const char *)lin + 2, 2);
      memcpy(&s->sin6_flowinfo, (const char *)lin + 4, 4);
      memcpy(&s->sin6_addr, (const char *)lin + 8, 16);
      *outlen = sizeof(*s);
      return 0;
   }
   return -1;
}
static void
bn_from_bsd(const struct sockaddr *bsd, void *lin, socklen_t *linlen)
{
   if (!lin || !linlen)
      return;
   uint8_t tmp[28] = {0};
   socklen_t n = 0;
   if (bsd->sa_family == AF_INET) {
      const struct sockaddr_in *s = (const struct sockaddr_in *)bsd;
      const uint16_t fam = 2;
      memcpy(tmp, &fam, 2);
      memcpy(tmp + 2, &s->sin_port, 2);
      memcpy(tmp + 4, &s->sin_addr, 4);
      n = 16;
   } else if (bsd->sa_family == AF_INET6) {
      const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)bsd;
      const uint16_t fam = 10;
      memcpy(tmp, &fam, 2);
      memcpy(tmp + 2, &s->sin6_port, 2);
      memcpy(tmp + 4, &s->sin6_flowinfo, 4);
      memcpy(tmp + 8, &s->sin6_addr, 16);
      n = 28;
   }
   const socklen_t copy = *linlen < n ? *linlen : n;
   memcpy(lin, tmp, copy);
   *linlen = n;
}

static int
bn_msg_flags(int lf)
{
   int f = 0;
   if (lf & 0x1) f |= MSG_OOB;
   if (lf & 0x2) f |= MSG_PEEK;
   if (lf & 0x40) f |= MSG_DONTWAIT;
   if (lf & 0x100) f |= MSG_WAITALL;
   if (lf & 0x4000) f |= MSG_NOSIGNAL;
   return f;
}

/* ---- socket / connect ------------------------------------------------------ */
static int
bn_socket(int domain, int type, int proto)
{
   const int base = type & 0xf;
   if (domain != 2 || (base != 1 && base != 2)) {
      static unsigned said;
      if (said++ < 8)
         bionic_say("socket(%d, 0x%x, %d): only IPv4 TCP/UDP -> EAFNOSUPPORT", domain, type, proto);
      errno = 97;
      return -1;
   }
   const int fd = socket(AF_INET, base == 1 ? SOCK_STREAM : SOCK_DGRAM, proto);
   if (fd < 0)
      BN_FAIL(-1);
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
   BN_T("socket(%d, 0x%x, %d) = %d", domain, type, proto, fd);
   if (fd < BN_FDS) {
      memset(&bn_fd[fd], 0, sizeof(bn_fd[fd]));
      bn_fd[fd].used = 1;
      bn_fd[fd].tcp = base == 1;
      if (type & 04000)
         bn_set_nonblock(fd, 1);
   }
   return fd;
}

static int
bn_io_all(int fd, void *buf, size_t n, int writing)
{
   size_t done = 0;
   while (done < n) {
      const ssize_t k = writing ? send(fd, (const char *)buf + done, n - done, MSG_NOSIGNAL)
                                : recv(fd, (char *)buf + done, n - done, 0);
      if (k <= 0) {
         if (k < 0 && errno == EINTR)
            continue;
         return -1;
      }
      done += (size_t)k;
   }
   return 0;
}

static int
bn_gateway_addr(struct sockaddr_in *gw)
{
   memset(gw, 0, sizeof(*gw));
   gw->sin_len = sizeof(*gw);
   gw->sin_family = AF_INET;
   const char *e = getenv("ROBLOX_GATEWAY");
   unsigned short port;
   if (gateway_config_parse(e, &gw->sin_addr, &port) != 0) {
      errno = EINVAL;
      return -1;
   }
   gw->sin_port = htons(port);
   return 0;
}


/* Real IPv4 address of a name, looked up by the gateway (its DNS-over-HTTPS
 * through WARP). The engine refuses private/reserved addresses, so it has to
 * see real ones. 0 on failure. Network order. */
#define BN_RES 512
static struct { char name[128]; uint32_t addr; uint64_t until; } bn_res[BN_RES];
static unsigned bn_res_n;
static uint32_t
bn_resolve(const char *name)
{
   const uint64_t now = (uint64_t)time(NULL);
   pthread_mutex_lock(&bn_lock);
   for (unsigned i = 0; i < bn_res_n; i++)
      if (strcmp(bn_res[i].name, name) == 0 && bn_res[i].until > now) {
         const uint32_t a = bn_res[i].addr;
         pthread_mutex_unlock(&bn_lock);
         return a;
      }
   pthread_mutex_unlock(&bn_lock);
   struct sockaddr_in gw;
   if (bn_gateway_addr(&gw) != 0)
      return 0;
   const int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return 0;
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
   struct timeval tv = {15, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   uint32_t addr = 0;
   uint8_t req[300], rep[16];
   const size_t l = strlen(name);
   if (l < 250 && connect(fd, (struct sockaddr *)&gw, sizeof(gw)) == 0 &&
       bn_io_all(fd, (uint8_t[]){5, 1, 0}, 3, 1) == 0 && bn_io_all(fd, rep, 2, 0) == 0 && rep[1] == 0) {
      size_t n = 0;
      req[n++] = 5, req[n++] = 0xF0, req[n++] = 0, req[n++] = 3, req[n++] = (uint8_t)l;
      memcpy(req + n, name, l);
      n += l;
      req[n++] = 0, req[n++] = 0;
      if (bn_io_all(fd, req, n, 1) == 0 && bn_io_all(fd, rep, 10, 0) == 0 && rep[1] == 0 && rep[3] == 1)
         memcpy(&addr, rep + 4, 4);
   }
   close(fd);
   if (addr) {
      pthread_mutex_lock(&bn_lock);
      unsigned i;
      for (i = 0; i < bn_res_n; i++)
         if (strcmp(bn_res[i].name, name) == 0)
            break;
      if (i == bn_res_n && bn_res_n < BN_RES)
         bn_res_n++;
      if (i < BN_RES) {
         snprintf(bn_res[i].name, sizeof(bn_res[i].name), "%s", name);
         bn_res[i].addr = addr;
         bn_res[i].until = now + 600;
      }
      pthread_mutex_unlock(&bn_lock);
   }
   return addr;
}
/* the name an address was looked up for (connect asks the gateway by name) */
static const char *
bn_name_for_addr(uint32_t addr /* network order */)
{
   const char *r = NULL;
   pthread_mutex_lock(&bn_lock);
   for (unsigned i = 0; i < bn_res_n; i++)
      if (bn_res[i].addr == addr) {
         r = bn_res[i].name;
         break;
      }
   pthread_mutex_unlock(&bn_lock);
   return r;
}

static unsigned bn_connects, bn_connect_fails;

static int
bn_connect(int fd, const void *addr, socklen_t len)
{
   struct sockaddr_storage ss;
   socklen_t sl;
   if (bn_to_bsd(addr, len, &ss, &sl) != 0) {
      errno = 97;
      return -1;
   }
   const bool tcp = fd >= 0 && fd < BN_FDS && bn_fd[fd].used && bn_fd[fd].tcp;
   if (!tcp || ss.ss_family != AF_INET) {
      if (connect(fd, (struct sockaddr *)&ss, sl) != 0)
         BN_FAIL(-1);
      return 0;
   }
   const struct sockaddr_in *dst = (const struct sockaddr_in *)&ss;
   const uint32_t host = ntohl(dst->sin_addr.s_addr);
   const char *name = bn_name_of(host);
   if (!name)
      name = bn_name_for_addr(dst->sin_addr.s_addr);
   bn_fd[fd].peer_addr = dst->sin_addr.s_addr;
   bn_fd[fd].peer_port = dst->sin_port;
   /* to the gateway, blocking for the handshake */
   const int nb = bn_fd[fd].nonblock;
   if (nb) { int z = 0; setsockopt(fd, SOL_SOCKET, BN_SO_NBIO, &z, sizeof(z)); }
   struct timeval tv = {20, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   struct sockaddr_in gw;
   int rc = -1, err = ECONNREFUSED;
   if (bn_gateway_addr(&gw) == 0 && connect(fd, (struct sockaddr *)&gw, sizeof(gw)) == 0) {
      uint8_t req[300], rep[300];
      size_t n = 0;
      if (bn_io_all(fd, (uint8_t[]){5, 1, 0}, 3, 1) == 0 && bn_io_all(fd, rep, 2, 0) == 0 && rep[0] == 5 &&
          rep[1] == 0) {
         req[n++] = 5, req[n++] = 1, req[n++] = 0;
         if (name) {
            const size_t l = strlen(name);
            req[n++] = 3, req[n++] = (uint8_t)l;
            memcpy(req + n, name, l);
            n += l;
         } else {
            req[n++] = 1;
            memcpy(req + n, &dst->sin_addr, 4);
            n += 4;
         }
         memcpy(req + n, &dst->sin_port, 2);
         n += 2;
         if (bn_io_all(fd, req, n, 1) == 0 && bn_io_all(fd, rep, 10, 0) == 0 && rep[1] == 0)
            rc = 0;
         else
            err = ECONNREFUSED;
      }
   } else {
      err = errno;
   }
   struct timeval none = {0, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
   if (nb) { int o = 1; setsockopt(fd, SOL_SOCKET, BN_SO_NBIO, &o, sizeof(o)); }
   const unsigned c = __atomic_add_fetch(&bn_connects, 1, __ATOMIC_RELAXED);
   char shown[64];
   inet_ntop(AF_INET, &dst->sin_addr, shown, sizeof(shown));
   if (rc != 0)
      __atomic_add_fetch(&bn_connect_fails, 1, __ATOMIC_RELAXED);
   if (c <= 40 || rc != 0)
      bionic_say("net: connect #%u %s:%u via gateway -> %s", c, name ? name : shown, ntohs(dst->sin_port),
                 rc == 0 ? "connected" : "FAILED");
   if (rc != 0) {
      errno = bn_linux_errno(err);
      return -1;
   }
   bn_fd[fd].via_gateway = 1;
   return 0;
}

static int bn_bind(int fd, const void *a, socklen_t l)
{
   struct sockaddr_storage ss;
   socklen_t sl;
   if (bn_to_bsd(a, l, &ss, &sl) != 0) {
      errno = 97;
      return -1;
   }
   if (bind(fd, (struct sockaddr *)&ss, sl) != 0)
      BN_FAIL(-1);
   return 0;
}

static ssize_t
bn_sendto(int fd, const void *b, size_t n, int lf, const void *a, socklen_t l)
{
   ssize_t r;
   if (a && l) {
      struct sockaddr_storage ss;
      socklen_t sl;
      if (bn_to_bsd(a, l, &ss, &sl) != 0) {
         errno = 97;
         return -1;
      }
      r = sendto(fd, b, n, bn_msg_flags(lf) | MSG_NOSIGNAL | bn_dontwait(fd), (struct sockaddr *)&ss, sl);
   } else {
      r = send(fd, b, n, bn_msg_flags(lf) | MSG_NOSIGNAL | bn_dontwait(fd));
   }
   if (r < 0)
      BN_FAIL(-1);
   return r;
}
static ssize_t bn_sendto_chk(int fd, const void *b, size_t n, size_t bn, int lf, const void *a, socklen_t l)
{
   (void)bn;
   return bn_sendto(fd, b, n, lf, a, l);
}
static ssize_t
bn_recvfrom(int fd, void *b, size_t n, int lf, void *a, socklen_t *l)
{
   struct sockaddr_storage ss;
   socklen_t sl = sizeof(ss);
   const ssize_t r = recvfrom(fd, b, n, bn_msg_flags(lf) | bn_dontwait(fd), a ? (struct sockaddr *)&ss : NULL, a ? &sl : NULL);
   if (r < 0)
      BN_FAIL(-1);
   if (a && l) {
      if (fd >= 0 && fd < BN_FDS && bn_fd[fd].via_gateway) {
         uint8_t lin[16] = {2, 0};
         memcpy(lin + 2, &bn_fd[fd].peer_port, 2);
         memcpy(lin + 4, &bn_fd[fd].peer_addr, 4);
         memcpy(a, lin, *l < 16 ? *l : 16);
         *l = 16;
      } else {
         bn_from_bsd((struct sockaddr *)&ss, a, l);
      }
   }
   return r;
}

/* Linux struct msghdr matches FreeBSD's offsets for the values used here
 * (little-endian, lengths small); only the address needs translating. */
struct bn_msghdr {
   void *name;
   uint32_t namelen, pad0;
   struct iovec *iov;
   size_t iovlen;
   void *control;
   size_t controllen;
   int flags;
};
static ssize_t
bn_sendmsg(int fd, const struct bn_msghdr *m, int lf)
{
   struct sockaddr_storage ss;
   socklen_t sl = 0;
   if (m->name && m->namelen && bn_to_bsd(m->name, m->namelen, &ss, &sl) != 0) {
      errno = 97;
      return -1;
   }
   struct msghdr h;
   memset(&h, 0, sizeof(h));
   h.msg_name = sl ? &ss : NULL;
   h.msg_namelen = sl;
   h.msg_iov = m->iov;
   h.msg_iovlen = (int)m->iovlen;
   const ssize_t r = sendmsg(fd, &h, bn_msg_flags(lf) | MSG_NOSIGNAL | bn_dontwait(fd));
   if (r < 0)
      BN_FAIL(-1);
   return r;
}
static ssize_t
bn_recvmsg(int fd, struct bn_msghdr *m, int lf)
{
   struct sockaddr_storage ss;
   struct msghdr h;
   memset(&h, 0, sizeof(h));
   h.msg_name = m->name ? &ss : NULL;
   h.msg_namelen = m->name ? sizeof(ss) : 0;
   h.msg_iov = m->iov;
   h.msg_iovlen = (int)m->iovlen;
   const ssize_t r = recvmsg(fd, &h, bn_msg_flags(lf) | bn_dontwait(fd));
   if (r < 0)
      BN_FAIL(-1);
   if (m->name) {
      socklen_t l = m->namelen;
      bn_from_bsd((struct sockaddr *)&ss, m->name, &l);
      m->namelen = l;
   }
   m->controllen = 0;
   m->flags = 0;
   return r;
}


/* recvmmsg / sendmmsg (Linux; the PS5 has neither): a loop of single
 * messages. struct mmsghdr = {struct msghdr (56 bytes); unsigned len} = 64.
 * Unprovided, recvmmsg answered 0 ("no messages") and a UDP thread spun on
 * it hundreds of millions of times. */
struct bn_mmsghdr {
   struct bn_msghdr hdr;
   unsigned len;
   unsigned pad;
};
static int
bn_recvmmsg(int fd, struct bn_mmsghdr *v, unsigned n, int lf, void *timeout)
{
   (void)timeout;
   unsigned got = 0;
   for (; got < n; got++) {
      /* the first one as asked; then only what is already there
       * (MSG_WAITFORONE, 0x10000, is how callers ask for that anyway) */
      const int flags = got ? (lf | 0x40) : lf;
      const ssize_t r = bn_recvmsg(fd, &v[got].hdr, flags);
      if (r < 0) {
         if (got)
            break;
         return -1;
      }
      v[got].len = (unsigned)r;
   }
   return (int)got;
}
static int
bn_sendmmsg(int fd, struct bn_mmsghdr *v, unsigned n, int lf)
{
   unsigned sent = 0;
   for (; sent < n; sent++) {
      const ssize_t r = bn_sendmsg(fd, &v[sent].hdr, lf);
      if (r < 0) {
         if (sent)
            break;
         return -1;
      }
      v[sent].len = (unsigned)r;
   }
   return (int)sent;
}

/* ---- options --------------------------------------------------------------- */
static int
bn_sockopt_map(int level, int name, int *blevel, int *bname)
{
   if (level == 1) { /* SOL_SOCKET */
      *blevel = SOL_SOCKET;
      switch (name) {
      case 2: *bname = SO_REUSEADDR; return 0;
      case 4: *bname = SO_ERROR; return 0;
      case 6: *bname = SO_BROADCAST; return 0;
      case 7: *bname = SO_SNDBUF; return 0;
      case 8: *bname = SO_RCVBUF; return 0;
      case 9: *bname = SO_KEEPALIVE; return 0;
      case 13: *bname = SO_LINGER; return 0;
      case 15: *bname = SO_REUSEPORT; return 0;
      case 20: *bname = SO_RCVTIMEO; return 0;
      case 21: *bname = SO_SNDTIMEO; return 0;
      case 3: *bname = SO_TYPE; return 0;
      default: return -1;
      }
   }
   if (level == 6) { /* IPPROTO_TCP */
      *blevel = IPPROTO_TCP;
      switch (name) {
      case 1: *bname = TCP_NODELAY; return 0;
      case 4: *bname = TCP_KEEPIDLE; return 0;
      case 5: *bname = TCP_KEEPINTVL; return 0;
      case 6: *bname = TCP_KEEPCNT; return 0;
      default: return -1;
      }
   }
   if (level == 0) { /* IPPROTO_IP */
      *blevel = IPPROTO_IP;
      switch (name) {
      case 1: *bname = IP_TOS; return 0;
      case 2: *bname = IP_TTL; return 0;
      default: return -1;
      }
   }
   return -1;
}
static int
bn_setsockopt(int fd, int level, int name, const void *v, socklen_t l)
{
   int bl, bnm;
   const int iv = v && l >= 4 ? *(const int *)v : -1;
   if (bn_sockopt_map(level, name, &bl, &bnm) != 0) {
      BN_T("setsockopt(%d, %d, %d, %d) unmapped -> 0", fd, level, name, iv);
      return 0; /* options the PS5 lacks: accepted and ignored */
   }
   const int r = setsockopt(fd, bl, bnm, v, l);
   BN_T("setsockopt(%d, %d, %d, %d) -> %d errno %d (reported 0)", fd, level, name, iv, r, r ? errno : 0);
   return 0;
}
static int
bn_getsockopt(int fd, int level, int name, void *v, socklen_t *l)
{
   int bl, bnm;
   if (bn_sockopt_map(level, name, &bl, &bnm) != 0) {
      if (v && l && *l >= 4) {
         memset(v, 0, 4);
         *l = 4;
      }
      return 0;
   }
   if (getsockopt(fd, bl, bnm, v, l) != 0) {
      BN_T("getsockopt(%d, %d, %d) failed errno %d", fd, level, name, errno);
      BN_FAIL(-1);
   }
   BN_T("getsockopt(%d, %d, %d) = %d", fd, level, name, v && *l >= 4 ? *(int *)v : -1);
   if (level == 1 && name == 4 && v && *l >= 4) { /* SO_ERROR value: Linux errno */
      int e;
      memcpy(&e, v, 4);
      e = bn_linux_errno(e);
      memcpy(v, &e, 4);
   }
   if (level == 1 && name == 3 && v && *l >= 4) { /* SO_TYPE: STREAM 1, DGRAM 2 agree */
   }
   return 0;
}
static int
bn_getsockname(int fd, void *a, socklen_t *l)
{
   BN_T("getsockname(%d)", fd);
   struct sockaddr_storage ss;
   socklen_t sl = sizeof(ss);
   if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
      BN_FAIL(-1);
   bn_from_bsd((struct sockaddr *)&ss, a, l);
   return 0;
}
static int
bn_getpeername(int fd, void *a, socklen_t *l)
{
   if (fd >= 0 && fd < BN_FDS && bn_fd[fd].via_gateway) {
      uint8_t lin[16] = {2, 0};
      memcpy(lin + 2, &bn_fd[fd].peer_port, 2);
      memcpy(lin + 4, &bn_fd[fd].peer_addr, 4);
      memcpy(a, lin, *l < 16 ? *l : 16);
      *l = 16;
      return 0;
   }
   struct sockaddr_storage ss;
   socklen_t sl = sizeof(ss);
   if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0)
      BN_FAIL(-1);
   bn_from_bsd((struct sockaddr *)&ss, a, l);
   return 0;
}
static int bn_shutdown(int fd, int how)
{
   if (shutdown(fd, how) != 0)
      BN_FAIL(-1);
   return 0;
}
static int
bn_ioctl(int fd, unsigned long req, ...)
{
   va_list ap;
   va_start(ap, req);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   BN_T("ioctl(%d, 0x%lx)", fd, req);
   if (req == 0x541B) { /* FIONREAD */
      if (ioctl(fd, FIONREAD, arg) != 0)
         BN_FAIL(-1);
      return 0;
   }
   if (req == 0x5421) { /* FIONBIO */
      const int on = arg ? *(int *)arg : 0;
      if (bn_is_socket(fd))
         return bn_set_nonblock(fd, on);
      const int f = fcntl(fd, F_GETFL);
      fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
      return 0;
   }
   errno = 25; /* ENOTTY */
   return -1;
}

/* ---- names ----------------------------------------------------------------- */
struct bn_addrinfo { /* Bionic's (BSD order: canonname before addr) */
   int ai_flags, ai_family, ai_socktype, ai_protocol;
   uint32_t ai_addrlen;
   char *ai_canonname;
   void *ai_addr;
   struct bn_addrinfo *ai_next;
};
static int
bn_getaddrinfo(const char *node, const char *service, const struct bn_addrinfo *hints, struct bn_addrinfo **res)
{
   if (res)
      *res = NULL;
   const int family = hints ? hints->ai_family : 0;
   if (family == 10) /* IPv6 only: none */
      return 8;      /* EAI_NONAME (Bionic) */
   uint32_t addr; /* network order */
   if (!node) {
      addr = htonl((hints && (hints->ai_flags & 1)) ? INADDR_ANY : INADDR_LOOPBACK);
   } else if (inet_pton(AF_INET, node, &addr) != 1) {
      if (strcmp(node, "localhost") == 0) {
         addr = htonl(INADDR_LOOPBACK);
      } else {
         addr = bn_resolve(node);
         if (!addr) {
            bionic_say("net: getaddrinfo(%s): the gateway could not resolve it", node);
            return 7; /* EAI_NODATA-ish: Bionic EAI_NONAME is 8 */
         }
      }
   }
   uint16_t port = 0;
   if (service)
      port = htons((uint16_t)atoi(service));
   const int types[2] = {1, 2};
   struct bn_addrinfo *head = NULL, **tail = &head;
   for (int k = 0; k < 2; k++) {
      if (hints && hints->ai_socktype && hints->ai_socktype != types[k])
         continue;
      struct bn_addrinfo *ai = calloc(1, sizeof(*ai) + 16);
      if (!ai)
         break;
      uint8_t *sa = (uint8_t *)(ai + 1);
      sa[0] = 2;
      memcpy(sa + 2, &port, 2);
      memcpy(sa + 4, &addr, 4);
      ai->ai_family = 2;
      ai->ai_socktype = types[k];
      ai->ai_protocol = types[k] == 1 ? 6 : 17;
      ai->ai_addrlen = 16;
      ai->ai_addr = sa;
      *tail = ai;
      tail = &ai->ai_next;
   }
   if (res)
      *res = head;
   static unsigned said;
   if (said++ < 30 && node)
      bionic_say("net: getaddrinfo(%s) -> %s (real address, via the gateway)", node, inet_ntoa(*(struct in_addr *)&addr));
   return head ? 0 : 8;
}
static void
bn_freeaddrinfo(struct bn_addrinfo *ai)
{
   while (ai) {
      struct bn_addrinfo *n = ai->ai_next;
      free(ai->ai_canonname);
      free(ai);
      ai = n;
   }
}
static const char *bn_gai_strerror(int e) { (void)e; return "name lookup failed"; }
static int
bn_inet_pton(int af, const char *src, void *dst)
{
   if (af == 2)
      return inet_pton(AF_INET, src, dst);
   if (af == 10)
      return inet_pton(AF_INET6, src, dst);
   errno = 97;
   return -1;
}
static const char *
bn_inet_ntop(int af, const void *src, char *dst, socklen_t n)
{
   const char *r = inet_ntop(af == 10 ? AF_INET6 : AF_INET, src, dst, n);
   BN_T("inet_ntop(%d) = %s", af, r ? r : "(null)");
   if (!r)
      errno = bn_linux_errno(errno);
   return r;
}
static int
bn_getnameinfo(const void *sa, socklen_t sl, char *host, socklen_t hl, char *serv, socklen_t svl, int flags)
{
   (void)sl;
   (void)flags;
   uint8_t lin[16];
   memcpy(lin, sa, 8);
   uint16_t port;
   memcpy(&port, lin + 2, 2);
   if (host && hl)
      inet_ntop(AF_INET, lin + 4, host, hl);
   if (serv && svl)
      snprintf(serv, svl, "%u", ntohs(port));
   return 0;
}

/* ---- poll (Linux POLLRDNORM 0x40 / POLLWRNORM 0x100) and select ---------- */
static int
bn_poll(struct pollfd *p, nfds_t n, int timeout)
{
   short saved[64];
   const nfds_t k = n < 64 ? n : 64;
   for (nfds_t i = 0; i < k; i++) {
      saved[i] = p[i].events;
      short e = p[i].events & (POLLIN | POLLPRI | POLLOUT);
      if (p[i].events & 0x40) e |= POLLIN;
      if (p[i].events & 0x100) e |= POLLOUT;
      p[i].events = e;
   }
   const int r = poll(p, n, timeout);
   for (nfds_t i = 0; i < k; i++) {
      short re = p[i].revents & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
      if ((p[i].revents & POLLIN) && (saved[i] & 0x40)) re |= 0x40;
      if ((p[i].revents & POLLOUT) && (saved[i] & 0x100)) re |= 0x100;
      p[i].revents = re;
      p[i].events = saved[i];
   }
   if (r < 0)
      BN_FAIL(-1);
   return r;
}

/* ---- epoll on poll ------------------------------------------------------------ */
struct bn_epoll_event { /* Linux x86_64: packed, 12 bytes */
   uint32_t events;
   uint64_t data;
} __attribute__((packed));
#define BN_EP_MAX 256
struct bn_epoll {
   int fds[BN_EP_MAX];
   uint32_t events[BN_EP_MAX];
   uint64_t data[BN_EP_MAX];
   int count;
   pthread_mutex_t lock;
};
static struct bn_epoll *bn_epolls[BN_FDS];

static int
bn_epoll_create1(int flags)
{
   (void)flags;
   int pp[2]; /* a descriptor of its own (a pipe's read end), never waited on */
   if (pipe(pp) != 0)
      BN_FAIL(-1);
   close(pp[1]);
   const int fd = pp[0];
   if (fd >= BN_FDS) {
      close(fd);
      errno = 24;
      return -1;
   }
   struct bn_epoll *e = calloc(1, sizeof(*e));
   pthread_mutex_init(&e->lock, NULL);
   bn_epolls[fd] = e;
   return fd;
}
static int bn_epoll_create(int size) { (void)size; return bn_epoll_create1(0); }
static int
bn_epoll_ctl(int epfd, int op, int fd, const struct bn_epoll_event *ev)
{
   struct bn_epoll *e = epfd >= 0 && epfd < BN_FDS ? bn_epolls[epfd] : NULL;
   if (!e) {
      errno = 9;
      return -1;
   }
   pthread_mutex_lock(&e->lock);
   int i;
   for (i = 0; i < e->count; i++)
      if (e->fds[i] == fd)
         break;
   int rc = 0;
   if (op == 1) { /* ADD */
      if (i < e->count) {
         errno = 17, rc = -1;
      } else if (e->count >= BN_EP_MAX) {
         errno = 28, rc = -1;
      } else {
         e->fds[e->count] = fd, e->events[e->count] = ev->events, e->data[e->count] = ev->data, e->count++;
      }
   } else if (op == 3) { /* MOD */
      if (i == e->count)
         errno = 2, rc = -1;
      else
         e->events[i] = ev->events, e->data[i] = ev->data;
   } else if (op == 2) { /* DEL */
      if (i == e->count) {
         errno = 2, rc = -1;
      } else {
         e->count--;
         e->fds[i] = e->fds[e->count], e->events[i] = e->events[e->count], e->data[i] = e->data[e->count];
      }
   } else {
      errno = 22, rc = -1;
   }
   pthread_mutex_unlock(&e->lock);
   return rc;
}
static int
bn_epoll_wait(int epfd, struct bn_epoll_event *out, int max, int timeout)
{
   struct bn_epoll *e = epfd >= 0 && epfd < BN_FDS ? bn_epolls[epfd] : NULL;
   if (!e || max <= 0) {
      errno = e ? 22 : 9;
      return -1;
   }
   struct pollfd p[BN_EP_MAX];
   uint64_t data[BN_EP_MAX];
   uint32_t want[BN_EP_MAX];
   int idx[BN_EP_MAX];
   pthread_mutex_lock(&e->lock);
   int n = 0;
   for (int i = 0; i < e->count; i++) {
      if (!(e->events[i] & 0x3fffffff))
         continue; /* oneshot already fired */
      p[n].fd = e->fds[i];
      p[n].events = (short)(((e->events[i] & 1) ? POLLIN : 0) | ((e->events[i] & 4) ? POLLOUT : 0) |
                            ((e->events[i] & 2) ? POLLPRI : 0));
      p[n].revents = 0;
      data[n] = e->data[i];
      want[n] = e->events[i];
      idx[n] = i;
      n++;
   }
   pthread_mutex_unlock(&e->lock);
   /* the read ends of eventfds are pipes: poll sees them directly */
   const int r = poll(p, (nfds_t)n, timeout);
   if (r < 0)
      BN_FAIL(-1);
   int got = 0;
   for (int i = 0; i < n && got < max; i++) {
      if (!p[i].revents)
         continue;
      uint32_t ev = 0;
      if (p[i].revents & POLLIN) ev |= 1;
      if (p[i].revents & POLLPRI) ev |= 2;
      if (p[i].revents & POLLOUT) ev |= 4;
      if (p[i].revents & POLLERR) ev |= 8;
      if (p[i].revents & POLLHUP) ev |= 0x10 | (want[i] & 0x2000);
      ev &= want[i] | 8 | 0x10;
      if (!ev)
         continue;
      out[got].events = ev;
      out[got].data = data[i];
      got++;
      if (want[i] & (1u << 30)) { /* EPOLLONESHOT: off until MOD */
         pthread_mutex_lock(&e->lock);
         if (idx[i] < e->count && e->fds[idx[i]] == p[i].fd)
            e->events[idx[i]] &= ~0x3fffffffu;
         pthread_mutex_unlock(&e->lock);
      }
   }
   return got;
}

static int
bn_select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv)
{
   /* Linux and FreeBSD fd_set are both bit arrays of longs */
   const int rc = select(n, r, w, x, tv);
   if (rc < 0)
      BN_FAIL(-1);
   return rc;
}

/* called from close(): forget the socket / epoll state of a descriptor */
static void
bn_forget(int fd)
{
   if (fd >= 0 && fd < BN_FDS && bn_fd[fd].used)
      BN_T("close(%d) of a socket", fd);
   if (fd >= 0 && fd < BN_FDS) {
      bn_fd[fd].used = 0;
      bn_fd[fd].via_gateway = 0;
      if (bn_epolls[fd]) {
         pthread_mutex_destroy(&bn_epolls[fd]->lock);
         free(bn_epolls[fd]);
         bn_epolls[fd] = NULL;
      }
   }
}

static void
bn_trace_fcntl(int fd, int cmd, long arg)
{
   if (fd >= 0 && fd < BN_FDS && bn_fd[fd].used)
      BN_T("fcntl(%d, %d, 0x%lx)", fd, cmd, arg);
}
