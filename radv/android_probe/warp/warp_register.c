/* On-console first-run WARP registration (see warp_register.h).
 * Uses only open/read/write/fsync/close/unlink/fstat: no link() and
 * no mkstemp(), which the PS5 title cannot import. Never logs anything. */
#include "warp_register.h"
#include "provision_prepare.h"
#include <bearssl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "../web_relay_tas.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef WARP_REGISTER_TEST
struct warp_register_test_hooks warp_register_test_hooks;
#endif

enum {
   WR_ARG = -1, WR_ENTROPY = -2, WR_PENDING = -3, WR_CONNECT = -4, WR_TLS_SETUP = -5,
   WR_TLS_IO = -6, WR_HTTP = -7, WR_REG_STATUS = -8, WR_REG_REPLY = -9, WR_PATCH = -10,
   WR_STORE = -11, WR_CLEANUP = -12,
};

#define WR_API_HOST "api.cloudflareclient.com"
#define WR_API_BASE "/v0a2158"
#define WR_RESP_CAP 16384     /* registration replies are ~1.5 KiB */
#define WR_HEADER_LINE 1024
#define WR_HEADER_LINES 64
#define WR_FILE_CAP 2048      /* the gateway's load_conf reads at most 2047 */
#define WR_JSON_DEPTH 16

static void wr_wipe(void *p, size_t n)
{
   volatile unsigned char *v = p;
   while (n--)
      *v++ = 0;
}

/* ---------------------------------------------------------------- JSON ---- */
/* Strict RFC 8259 syntax check plus exact-key lookup. Extracted values must
 * be plain strings without escapes, or small unsigned integers. */

static const char *j_ws(const char *p, const char *e)
{
   while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      p++;
   return p;
}

static int j_hex(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* *p at '"'; on success *p is just past the closing quote. */
static int j_string(const char **pp, const char *e)
{
   const char *p = *pp;
   if (p >= e || *p != '"')
      return -1;
   for (p++; p < e; p++) {
      const unsigned char c = (unsigned char)*p;
      if (c == '"') {
         *pp = p + 1;
         return 0;
      }
      if (c < 0x20)
         return -1;
      if (c == '\\') {
         if (++p >= e)
            return -1;
         if (*p == 'u') {
            if (e - p < 5 || !j_hex(p[1]) || !j_hex(p[2]) || !j_hex(p[3]) || !j_hex(p[4]))
               return -1;
            p += 4;
         } else if (!strchr("\"\\/bfnrt", *p) || !*p) {
            return -1;
         }
      }
   }
   return -1;
}

static int j_digits(const char **pp, const char *e)
{
   const char *p = *pp;
   while (p < e && *p >= '0' && *p <= '9')
      p++;
   if (p == *pp)
      return -1;
   *pp = p;
   return 0;
}

static int j_number(const char **pp, const char *e)
{
   const char *p = *pp;
   if (p < e && *p == '-')
      p++;
   if (p < e && *p == '0')
      p++;
   else if (p >= e || *p < '1' || *p > '9' || j_digits(&p, e))
      return -1;
   if (p < e && *p == '.') {
      p++;
      if (j_digits(&p, e))
         return -1;
   }
   if (p < e && (*p == 'e' || *p == 'E')) {
      p++;
      if (p < e && (*p == '+' || *p == '-'))
         p++;
      if (j_digits(&p, e))
         return -1;
   }
   *pp = p;
   return 0;
}

static int j_value(const char **pp, const char *e, int depth)
{
   const char *p = j_ws(*pp, e);
   if (p >= e || depth > WR_JSON_DEPTH)
      return -1;
   if (*p == '"') {
      if (j_string(&p, e))
         return -1;
   } else if (*p == '{' || *p == '[') {
      const char close = *p == '{' ? '}' : ']';
      const int object = *p == '{';
      p = j_ws(p + 1, e);
      if (p < e && *p == close) {
         p++;
      } else {
         for (;;) {
            if (object) {
               p = j_ws(p, e);
               if (j_string(&p, e))
                  return -1;
               p = j_ws(p, e);
               if (p >= e || *p++ != ':')
                  return -1;
            }
            if (j_value(&p, e, depth + 1))
               return -1;
            p = j_ws(p, e);
            if (p >= e)
               return -1;
            if (*p == ',') {
               p++;
               continue;
            }
            if (*p++ != close)
               return -1;
            break;
         }
      }
   } else if (e - p >= 4 && !memcmp(p, "true", 4)) {
      p += 4;
   } else if (e - p >= 5 && !memcmp(p, "false", 5)) {
      p += 5;
   } else if (e - p >= 4 && !memcmp(p, "null", 4)) {
      p += 4;
   } else if (j_number(&p, e)) {
      return -1;
   }
   *pp = p;
   return 0;
}

/* Whole document must be one object with nothing but whitespace after it. */
static int j_document(const char *s, size_t n)
{
   const char *p = j_ws(s, s + n);
   if (p >= s + n || *p != '{' || j_value(&p, s + n, 0))
      return -1;
   return j_ws(p, s + n) == s + n ? 0 : -1;
}

/* obj points at '{' of an already validated document. Duplicate keys are
 * rejected. */
static int j_member(const char *obj, const char *e, const char *key, const char **val)
{
   const size_t klen = strlen(key);
   const char *p = j_ws(obj, e), *found = NULL;
   if (p >= e || *p != '{')
      return -1;
   p = j_ws(p + 1, e);
   if (p < e && *p == '}')
      return -1;
   for (;;) {
      const char *k = j_ws(p, e);
      p = k;
      if (j_string(&p, e))
         return -1;
      const int match = (size_t)(p - k) == klen + 2 && !memcmp(k + 1, key, klen);
      p = j_ws(p, e);
      if (p >= e || *p++ != ':')
         return -1;
      const char *v = j_ws(p, e);
      if (j_value(&p, e, 0))
         return -1;
      if (match) {
         if (found)
            return -1;
         found = v;
      }
      p = j_ws(p, e);
      if (p < e && *p == ',') {
         p++;
         continue;
      }
      break;
   }
   if (!found)
      return -1;
   *val = found;
   return 0;
}

static int j_index(const char *arr, const char *e, unsigned index, const char **val)
{
   const char *p = j_ws(arr, e);
   if (p >= e || *p != '[')
      return -1;
   p = j_ws(p + 1, e);
   if (p < e && *p == ']')
      return -1;
   for (unsigned i = 0;; i++) {
      const char *v = j_ws(p, e);
      if (j_value(&p, e, 0))
         return -1;
      if (i == index) {
         *val = v;
         return 0;
      }
      p = j_ws(p, e);
      if (p >= e || *p != ',')
         return -1;
      p++;
   }
}

/* Path like "config.peers.0.endpoint.v4": numeric segments index arrays. */
static int j_path(const char *doc, const char *e, const char *path, const char **val)
{
   char seg[32];
   const char *cur = doc;
   while (*path) {
      size_t n = strcspn(path, ".");
      if (!n || n >= sizeof(seg))
         return -1;
      memcpy(seg, path, n);
      seg[n] = 0;
      path += n + (path[n] == '.');
      if (seg[0] >= '0' && seg[0] <= '9') {
         if (n != 1 || j_index(cur, e, (unsigned)(seg[0] - '0'), &cur))
            return -1;
      } else if (j_member(cur, e, seg, &cur)) {
         return -1;
      }
   }
   *val = cur;
   return 0;
}

static int j_get_str(const char *doc, const char *e, const char *path, char *out, size_t cap)
{
   const char *v, *p;
   if (j_path(doc, e, path, &v) || v >= e || *v != '"')
      return -1;
   p = v;
   if (j_string(&p, e))
      return -1;
   const size_t len = (size_t)(p - v) - 2;
   if (!len || len >= cap || memchr(v + 1, '\\', len))
      return -1;
   memcpy(out, v + 1, len);
   out[len] = 0;
   return 0;
}

static int j_get_port(const char *doc, const char *e, const char *path, unsigned *out)
{
   const char *v;
   unsigned n = 0;
   if (j_path(doc, e, path, &v))
      return -1;
   const char *p = v;
   while (p < e && *p >= '0' && *p <= '9') {
      n = n * 10 + (unsigned)(*p - '0');
      if (n > 65535)
         return -1;
      p++;
   }
   if (p == v || (*v == '0' && p - v > 1) || (p < e && (*p == '.' || *p == 'e' || *p == 'E')))
      return -1;
   if (n < 1)
      return -1;
   *out = n;
   return 0;
}

/* ---------------------------------------------------------- validation ---- */

static int b64_val(char c)
{
   if (c >= 'A' && c <= 'Z') return c - 'A';
   if (c >= 'a' && c <= 'z') return c - 'a' + 26;
   if (c >= '0' && c <= '9') return c - '0' + 52;
   if (c == '+') return 62;
   if (c == '/') return 63;
   return -1;
}

/* Canonical standard base64 of exactly 32 bytes: 43 symbols then '='. */
static int b64_key_decode(const char *s, unsigned char out[32])
{
   unsigned char tmp[33];
   if (strlen(s) != 44 || s[43] != '=')
      return -1;
   size_t o = 0;
   for (size_t i = 0; i < 44; i += 4) {
      int v[4];
      for (int k = 0; k < 4; k++)
         v[k] = (i + (size_t)k == 43) ? 0 : b64_val(s[i + (size_t)k]);
      if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0) {
         wr_wipe(tmp, sizeof(tmp));
         return -1;
      }
      const unsigned w = (unsigned)v[0] << 18 | (unsigned)v[1] << 12 | (unsigned)v[2] << 6 | (unsigned)v[3];
      tmp[o++] = (unsigned char)(w >> 16);
      tmp[o++] = (unsigned char)(w >> 8);
      if (o < 33)
         tmp[o++] = (unsigned char)w;
   }
   const int canonical = (b64_val(s[42]) & 3) == 0;
   if (out && canonical)
      memcpy(out, tmp, 32);
   wr_wipe(tmp, sizeof(tmp));
   return canonical ? 0 : -1;
}

static int valid_ipv4(const char *s)
{
   struct in_addr a;
   return strlen(s) <= 15 && inet_pton(AF_INET, s, &a) == 1;
}

static int charset_ok(const char *s, const char *extra)
{
   for (; *s; s++) {
      const char c = *s;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr(extra, c)))
         return 0;
   }
   return 1;
}

struct wr_conf {
   char private_key[45], peer_public_key[45], endpoint_ip[16], address_v4[16];
   char id[65], token[257];
   unsigned endpoint_port;
};

static int conf_fields_ok(const struct wr_conf *c)
{
   return b64_key_decode(c->private_key, NULL) == 0 &&
          b64_key_decode(c->peer_public_key, NULL) == 0 &&
          valid_ipv4(c->endpoint_ip) && valid_ipv4(c->address_v4) &&
          c->endpoint_port >= 1 && c->endpoint_port <= 65535 &&
          c->id[0] && charset_ok(c->id, "-") &&
          c->token[0] && charset_ok(c->token, "-._~+/=");
}

static int conf_to_json(const struct wr_conf *c, char *out, size_t cap)
{
   const int n = snprintf(out, cap,
      "{\"private_key\":\"%s\",\"peer_public_key\":\"%s\",\"endpoint_ip\":\"%s\","
      "\"endpoint_port\":%u,\"address_v4\":\"%s\",\"dns\":\"1.1.1.1\",\"id\":\"%s\",\"token\":\"%s\"}\n",
      c->private_key, c->peer_public_key, c->endpoint_ip, c->endpoint_port, c->address_v4, c->id, c->token);
   return n > 0 && (size_t)n < cap ? n : -1;
}

static int conf_from_json(const char *s, size_t n, struct wr_conf *c)
{
   const char *e = s + n;
   memset(c, 0, sizeof(*c));
   if (j_document(s, n) ||
       j_get_str(s, e, "private_key", c->private_key, sizeof(c->private_key)) ||
       j_get_str(s, e, "peer_public_key", c->peer_public_key, sizeof(c->peer_public_key)) ||
       j_get_str(s, e, "endpoint_ip", c->endpoint_ip, sizeof(c->endpoint_ip)) ||
       j_get_port(s, e, "endpoint_port", &c->endpoint_port) ||
       j_get_str(s, e, "address_v4", c->address_v4, sizeof(c->address_v4)) ||
       j_get_str(s, e, "id", c->id, sizeof(c->id)) ||
       j_get_str(s, e, "token", c->token, sizeof(c->token)) ||
       !conf_fields_ok(c)) {
      wr_wipe(c, sizeof(*c));
      return -1;
   }
   return 0;
}

/* "a.b.c.d:port" with port 1..65535 (or 0, reported as 0 for the caller). */
static int split_endpoint(const char *ep, char ip[16], unsigned *port)
{
   const char *colon = strrchr(ep, ':');
   unsigned n = 0;
   if (!colon || colon == ep || (size_t)(colon - ep) > 15 || !colon[1] || strlen(colon + 1) > 5)
      return -1;
   memcpy(ip, ep, (size_t)(colon - ep));
   ip[colon - ep] = 0;
   if (!valid_ipv4(ip))
      return -1;
   for (const char *p = colon + 1; *p; p++) {
      if (*p < '0' || *p > '9')
         return -1;
      n = n * 10 + (unsigned)(*p - '0');
   }
   if (n > 65535 || (colon[1] == '0' && colon[2]))
      return -1;
   *port = n;
   return 0;
}

static int parse_registration(const char *s, size_t n, const char *private_key, struct wr_conf *c)
{
   const char *e = s + n;
   char endpoint[32];
   memset(c, 0, sizeof(*c));
   if (j_document(s, n) ||
       j_get_str(s, e, "id", c->id, sizeof(c->id)) ||
       j_get_str(s, e, "token", c->token, sizeof(c->token)) ||
       j_get_str(s, e, "config.peers.0.public_key", c->peer_public_key, sizeof(c->peer_public_key)) ||
       j_get_str(s, e, "config.peers.0.endpoint.v4", endpoint, sizeof(endpoint)) ||
       j_get_str(s, e, "config.interface.addresses.v4", c->address_v4, sizeof(c->address_v4)) ||
       split_endpoint(endpoint, c->endpoint_ip, &c->endpoint_port))
      goto bad;
   /* Some API versions publish the v4 address with port 0 and list the
    * usable ports separately; take the first listed port then. */
   if (c->endpoint_port == 0 && j_get_port(s, e, "config.peers.0.endpoint.ports.0", &c->endpoint_port))
      goto bad;
   memcpy(c->private_key, private_key, 45);
   if (!conf_fields_ok(c))
      goto bad;
   return 0;
bad:
   wr_wipe(c, sizeof(*c));
   return -1;
}

/* ---------------------------------------------------------------- files ---- */

static int read_private_file(const char *path, char *buf, size_t cap, size_t *len)
{
   struct stat st;
   const int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
      return errno == ENOENT ? 1 : -1;
   size_t done = 0;
   int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && (size_t)st.st_size < cap;
   while (ok && done < (size_t)st.st_size) {
      const ssize_t n = read(fd, buf + done, cap - 1 - done);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      done += (size_t)n;
   }
   close(fd);
   if (!ok || done != (size_t)st.st_size) {
      wr_wipe(buf, cap);
      return -1;
   }
   buf[done] = 0;
   *len = done;
   return 0;
}

static void sync_dir(const char *dir)
{
   const int fd = open(dir, O_RDONLY);
   if (fd >= 0) {
      (void)fsync(fd);
      close(fd);
   }
}

/* Pending file: {"private_key":..,"public_key":..}; the public key must be
 * the X25519 image of the private key. Returns the base64 keys. */
static int load_pending(const char *path, char priv[45], char pub[45])
{
   char buf[512];
   size_t n = 0;
   unsigned char sk[32], pk[32], want[32];
   int result = -1;
   if (read_private_file(path, buf, sizeof(buf), &n) != 0)
      goto out;
   const char *e = buf + n;
   if (j_document(buf, n) ||
       j_get_str(buf, e, "private_key", priv, 45) || j_get_str(buf, e, "public_key", pub, 45) ||
       b64_key_decode(priv, sk) || b64_key_decode(pub, want) || x25519_base(pk, sk, 1) != 0 ||
       memcmp(pk, want, 32))
      goto out;
   result = 0;
out:
   if (result) {
      wr_wipe(priv, 45);
      wr_wipe(pub, 45);
   }
   wr_wipe(buf, sizeof(buf));
   wr_wipe(sk, sizeof(sk));
   wr_wipe(pk, sizeof(pk));
   wr_wipe(want, sizeof(want));
   return result;
}

/* ------------------------------------------------------------------ TLS ---- */

struct wr_tls {
   br_ssl_client_context client;
   br_x509_minimal_context x509;
   br_sslio_context io;
   unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
   unsigned char rbuf[1024];
   size_t rpos, rlen;
   int fd;
};

static int wr_sock_read(void *ctx, unsigned char *buf, size_t len)
{
   ssize_t n;
   do { n = recv(*(int *)ctx, buf, len, 0); } while (n < 0 && errno == EINTR);
   return n > 0 ? (int)n : -1;
}

static int wr_sock_write(void *ctx, const unsigned char *buf, size_t len)
{
   ssize_t n;
   do { n = send(*(int *)ctx, buf, len, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
   return n > 0 ? (int)n : -1;
}

static int wr_connect_ip(const char *ip, unsigned short port)
{
   struct sockaddr_in dest;
   memset(&dest, 0, sizeof(dest));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
   dest.sin_len = sizeof(dest);
#endif
   dest.sin_family = AF_INET;
   dest.sin_port = htons(port);
   if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1)
      return -1;
   const int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct timeval timeout = {10, 0};
   (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) == 0)
      return fd;
   close(fd);
   return -1;
}

/* System DNS first; the fixed addresses are only a fallback: TLS still
 * verifies the API host name against trusted anchors. */
static int wr_connect(void)
{
#ifdef WARP_REGISTER_TEST
   if (warp_register_test_hooks.connect_ip)
      return wr_connect_ip(warp_register_test_hooks.connect_ip,
                           warp_register_test_hooks.port ? warp_register_test_hooks.port : 443);
#endif
   struct addrinfo hints, *addresses = NULL;
   int fd = -1;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(WR_API_HOST, "443", &hints, &addresses) == 0) {
      for (struct addrinfo *a = addresses; a && fd < 0; a = a->ai_next) {
         char ip[INET_ADDRSTRLEN];
         if (a->ai_family == AF_INET &&
             inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof(ip)))
            fd = wr_connect_ip(ip, 443);
      }
      freeaddrinfo(addresses);
   }
   static const char *const fallback[] = {"104.16.192.82", "104.16.24.84"};
   for (size_t i = 0; fd < 0 && i < sizeof(fallback) / sizeof(fallback[0]); i++)
      fd = wr_connect_ip(fallback[i], 443);
   return fd;
}

static int wr_getc(struct wr_tls *t)
{
   if (t->rpos == t->rlen) {
      const int n = br_sslio_read(&t->io, t->rbuf, sizeof(t->rbuf));
      if (n <= 0)
         return -1;
      t->rpos = 0;
      t->rlen = (size_t)n;
   }
   return t->rbuf[t->rpos++];
}

/* One CRLF (or LF) terminated line without the terminator. */
static int wr_line(struct wr_tls *t, char *out, size_t cap)
{
   size_t n = 0;
   for (;;) {
      const int c = wr_getc(t);
      if (c < 0)
         return -1;
      if (c == '\n')
         break;
      if (n + 1 >= cap || c == 0)
         return -1;
      out[n++] = (char)c;
   }
   if (n && out[n - 1] == '\r')
      n--;
   out[n] = 0;
   return (int)n;
}

static int wr_read_exact(struct wr_tls *t, char *out, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      const int c = wr_getc(t);
      if (c < 0)
         return -1;
      out[i] = (char)c;
   }
   return 0;
}

static int wr_parse_size(const char *s, int hex, size_t *out)
{
   size_t n = 0;
   const char *p = s;
   for (; *p; p++) {
      int d;
      if (*p >= '0' && *p <= '9') d = *p - '0';
      else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
      else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
      else break;
      n = n * (hex ? 16 : 10) + (size_t)d;
      if (n > WR_RESP_CAP)
         return -1;
   }
   if (p == s)
      return -1;
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p && !(hex && *p == ';'))
      return -1;
   *out = n;
   return 0;
}

static int wr_ascii_ieq(const char *a, const char *b, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
      if (x != y)
         return 0;
      if (!x)
         return 1;
   }
   return 1;
}

static int wr_header_is(const char *line, const char *name, const char **value)
{
   const size_t n = strlen(name);
   if (!wr_ascii_ieq(line, name, n) || line[n] != ':')
      return 0;
   const char *v = line + n + 1;
   while (*v == ' ' || *v == '\t')
      v++;
   *value = v;
   return 1;
}

/* One HTTPS request on a fresh connection. Returns 0 with *status and the
 * NUL-terminated body in resp, or WR_CONNECT / WR_TLS_SETUP / WR_TLS_IO /
 * WR_HTTP. */
static int wr_https(const char *method, const char *path, const char *token, const char *body,
                    char *resp, size_t *resp_len, int *status)
{
   char request[1024], line[WR_HEADER_LINE];
   unsigned char seed[32];
   struct wr_tls *t = NULL;
   int fd = -1, result = WR_ENTROPY;
   if (warp_secure_random_from_path("/dev/urandom", seed, sizeof(seed)) != 0 &&
       warp_secure_random_from_path("/dev/random", seed, sizeof(seed)) != 0)
      goto out;
   result = WR_TLS_SETUP;
   const size_t body_len = strlen(body);
   const int req_len = snprintf(request, sizeof(request),
      "%s %s HTTP/1.1\r\nHost: " WR_API_HOST "\r\nUser-Agent: okhttp/3.12.1\r\n"
      "CF-Client-Version: a-6.11-2223\r\nContent-Type: application/json; charset=UTF-8\r\n"
      "Accept: application/json\r\n%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
      method, path, token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "",
      body_len, body);
   if (req_len <= 0 || (size_t)req_len >= sizeof(request))
      goto out;
   result = WR_CONNECT;
   if ((fd = wr_connect()) < 0)
      goto out;
   result = WR_TLS_SETUP;
   if (!(t = calloc(1, sizeof(*t))))
      goto out;
   t->fd = fd;
   const br_x509_trust_anchor *tas = TAs;
   size_t tas_num = TAs_NUM;
#ifdef WARP_REGISTER_TEST
   if (warp_register_test_hooks.tas) {
      tas = warp_register_test_hooks.tas;
      tas_num = warp_register_test_hooks.tas_num;
   }
#endif
   br_ssl_client_init_full(&t->client, &t->x509, tas, tas_num);
   br_ssl_engine_set_buffer(&t->client.eng, t->buffer, sizeof(t->buffer), 1);
   const time_t now = time(NULL);
   if (now < 1577836800)
      goto out;
   br_x509_minimal_set_time(&t->x509, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
   br_ssl_engine_inject_entropy(&t->client.eng, seed, sizeof(seed));
   if (!br_ssl_client_reset(&t->client, WR_API_HOST, 0))
      goto out;
   br_sslio_init(&t->io, &t->client.eng, wr_sock_read, &t->fd, wr_sock_write, &t->fd);
   result = WR_TLS_IO;
   if (br_sslio_write_all(&t->io, request, (size_t)req_len) != 0 || br_sslio_flush(&t->io) != 0)
      goto out;

   result = WR_HTTP;
   /* Status line: HTTP/1.x NNN[ reason] */
   if (wr_line(t, line, sizeof(line)) < 12 || strncmp(line, "HTTP/1.", 7) ||
       (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
       line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
       line[11] < '0' || line[11] > '9' || (line[12] && line[12] != ' '))
      goto out;
   const int code = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
   int chunked = 0, have_length = 0, lines = 0;
   size_t length = 0;
   for (;;) {
      const int n = wr_line(t, line, sizeof(line));
      if (n < 0 || ++lines > WR_HEADER_LINES)
         goto out;
      if (n == 0)
         break;
      const char *v;
      if (wr_header_is(line, "Content-Length", &v)) {
         size_t parsed;
         if (wr_parse_size(v, 0, &parsed) || (have_length && parsed != length))
            goto out;
         have_length = 1;
         length = parsed;
      } else if (wr_header_is(line, "Transfer-Encoding", &v)) {
         if (strlen(v) != 7 || !wr_ascii_ieq(v, "chunked", 7) || chunked)
            goto out;
         chunked = 1;
      }
   }
   if (chunked && have_length)
      goto out;
   size_t used = 0;
   if (code == 204 || code == 304) {
      if (chunked || (have_length && length))
         goto out;
   } else if (chunked) {
      for (;;) {
         size_t size;
         if (wr_line(t, line, sizeof(line)) < 0 || wr_parse_size(line, 1, &size))
            goto out;
         if (!size)
            break;
         if (size >= WR_RESP_CAP - used || wr_read_exact(t, resp + used, size))
            goto out;
         used += size;
         if (wr_line(t, line, sizeof(line)) != 0)
            goto out;
      }
      for (int trailers = 0;; trailers++) { /* trailer section */
         const int n = wr_line(t, line, sizeof(line));
         if (n < 0 || trailers > WR_HEADER_LINES)
            goto out;
         if (n == 0)
            break;
      }
   } else if (have_length) {
      if (length >= WR_RESP_CAP || wr_read_exact(t, resp, length))
         goto out;
      used = length;
   } else {
      /* Delimited by connection close: require a proper TLS close_notify so
       * a truncation cannot pass as a complete body. */
      for (int c; (c = wr_getc(t)) >= 0;) {
         if (used + 1 >= WR_RESP_CAP)
            goto out;
         resp[used++] = (char)c;
      }
      if (br_ssl_engine_last_error(&t->client.eng) != BR_ERR_OK)
         goto out;
   }
   resp[used] = 0;
   if (memchr(resp, 0, used))
      goto out;
   *resp_len = used;
   *status = code;
   result = 0;
out:
   wr_wipe(seed, sizeof(seed));
   wr_wipe(request, sizeof(request));
   wr_wipe(line, sizeof(line));
   if (t) {
      wr_wipe(t, sizeof(*t));
      free(t);
   }
   if (fd >= 0)
      close(fd);
   return result;
}

/* ----------------------------------------------------------------- flow ---- */

static void iso_time(time_t t, char out[48])
{
   long long days = (long long)t / 86400, secs = (long long)t % 86400;
   if (secs < 0) {
      secs += 86400;
      days--;
   }
   const long long z = days + 719468;
   const long long era = (z >= 0 ? z : z - 146096) / 146097;
   const unsigned doe = (unsigned)(z - era * 146097);
   const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
   const unsigned mp = (5 * doy + 2) / 153;
   const unsigned d = doy - (153 * mp + 2) / 5 + 1;
   const unsigned m = mp < 10 ? mp + 3 : mp - 9;
   const long long y = (long long)yoe + era * 400 + (m <= 2);
   snprintf(out, 48, "%04lld-%02u-%02uT%02u:%02u:%02u.000Z", y, m, d,
            (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

struct wr_work {
   char warp_path[1100], pending_path[1100], staged_path[1100];
   char priv[45], pub[45];
   char resp[WR_RESP_CAP];
   char file[WR_FILE_CAP];
   char body[512], path[160];
   struct wr_conf conf, other;
};

/* Remove pending/staged leftovers only if they hold the same private key
 * as the durable identity. */
static int cleanup(struct wr_work *w, const char *dir, const char *private_key)
{
   int bad = 0;
   size_t n;
   int r = read_private_file(w->staged_path, w->file, sizeof(w->file), &n);
   if (r == 0) {
      if (conf_from_json(w->file, n, &w->other) == 0 && !strcmp(w->other.private_key, private_key) &&
          unlink(w->staged_path) != 0 && errno != ENOENT)
         bad = 1;
      wr_wipe(&w->other, sizeof(w->other));
   }
   wr_wipe(w->file, sizeof(w->file));
   char priv[45] = {0}, pub[45] = {0};
   if (load_pending(w->pending_path, priv, pub) == 0 && !strcmp(priv, private_key) &&
       unlink(w->pending_path) != 0 && errno != ENOENT)
      bad = 1;
   wr_wipe(priv, sizeof(priv));
   wr_wipe(pub, sizeof(pub));
   sync_dir(dir);
   return bad ? WR_CLEANUP : 0;
}

/* 1 = the path exists, 0 = it does not (ENOENT), -1 = cannot tell.
 * open(), not lstat(): on the PS5 a title's lstat() of a missing file fails
 * with EPERM instead of ENOENT (seen Nov 18), which looked like an error. */
static int
wr_present(const char *path)
{
   const int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd >= 0) {
      close(fd);
      return 1;
   }
   return errno == ENOENT ? 0 : -1;
}

static int wr_run(struct wr_work *w, const char *dir)
{
   size_t n = 0;
   int r, status = 0;
   if (strlen(dir) > 1024)
      return WR_ARG;
   snprintf(w->warp_path, sizeof(w->warp_path), "%s/warp.json", dir);
   snprintf(w->pending_path, sizeof(w->pending_path), "%s/warp-pending.json", dir);
   snprintf(w->staged_path, sizeof(w->staged_path), "%s/warp-staged.json", dir);

   const int have_warp = wr_present(w->warp_path);
   if (have_warp == 1) {
      /* Never contact the network once an identity exists. Tidy leftovers
       * from an interrupted run if warp.json is valid and matches them. */
      if (read_private_file(w->warp_path, w->file, sizeof(w->file), &n) == 0 &&
          conf_from_json(w->file, n, &w->conf) == 0)
         (void)cleanup(w, dir, w->conf.private_key);
      return 1;
   }
   if (have_warp < 0)
      return WR_ARG;

   /* A registration already completed (POST) but not yet enabled/stored. */
   r = read_private_file(w->staged_path, w->file, sizeof(w->file), &n);
   int have_conf = r == 0 && conf_from_json(w->file, n, &w->conf) == 0;
   if (r == 0 && !have_conf && unlink(w->staged_path) != 0)
      return WR_STORE;      /* invalid staged file: drop it, register again */
   if (r < 0)
      return WR_STORE;

   if (!have_conf) {
      const int have_pending = wr_present(w->pending_path);
      if (have_pending < 0)
         return WR_PENDING;
      if (have_pending == 0) {
         const char *entropy = "/dev/urandom";
#ifdef WARP_REGISTER_TEST
         if (warp_register_test_hooks.entropy_path)
            entropy = warp_register_test_hooks.entropy_path;
#endif
         /* On failure nothing is left behind (O_EXCL create, removed on a
          * short write); a concurrent creator's file is simply reused. */
         if (warp_provision_prepare(entropy, w->pending_path) != 0 &&
             wr_present(w->pending_path) != 1 &&
             warp_provision_prepare("/dev/random", w->pending_path) != 0 &&
             wr_present(w->pending_path) != 1)
            return WR_ENTROPY;
      }
      if (load_pending(w->pending_path, w->priv, w->pub) != 0)
         return WR_PENDING;

      char tos[48];
      iso_time(time(NULL), tos);
      const int bl = snprintf(w->body, sizeof(w->body),
         "{\"key\":\"%s\",\"install_id\":\"\",\"fcm_token\":\"\",\"tos\":\"%s\","
         "\"model\":\"PlayStation 5\",\"serial_number\":\"\",\"locale\":\"en_US\"}", w->pub, tos);
      if (bl <= 0 || (size_t)bl >= sizeof(w->body))
         return WR_PENDING;
      r = wr_https("POST", WR_API_BASE "/reg", NULL, w->body, w->resp, &n, &status);
      if (r != 0)
         return r == WR_ENTROPY ? WR_TLS_SETUP : r;
      if (status < 200 || status > 299)
         return WR_REG_STATUS;
      if (parse_registration(w->resp, n, w->priv, &w->conf) != 0)
         return WR_REG_REPLY;
      wr_wipe(w->resp, sizeof(w->resp));
      const int fl = conf_to_json(&w->conf, w->file, sizeof(w->file));
      if (fl < 0 || warp_identity_create(w->staged_path, w->file, (size_t)fl) != 0)
         return WR_STORE;
   }

   snprintf(w->path, sizeof(w->path), WR_API_BASE "/reg/%s", w->conf.id);
   r = wr_https("PATCH", w->path, w->conf.token, "{\"warp_enabled\":true}", w->resp, &n, &status);
   if (r != 0)
      return WR_PATCH;
   if (status < 200 || status > 299) {
      /* The server rejected this registration's credentials: it can never
       * be enabled, so drop it; the pending key stays for a new POST. */
      if (status == 401 || status == 403 || status == 404)
         (void)unlink(w->staged_path);
      return WR_PATCH;
   }

   const int fl = conf_to_json(&w->conf, w->file, sizeof(w->file));
   if (fl < 0 || warp_identity_create(w->warp_path, w->file, (size_t)fl) != 0)
      return WR_STORE;
   /* warp.json is written and fsynced (file and directory). */
   return cleanup(w, dir, w->conf.private_key);
}

int warp_first_run_register(const char *dir)
{
   if (!dir || !*dir)
      return WR_ARG;
   struct wr_work *w = calloc(1, sizeof(*w));
   if (!w)
      return WR_ARG;
   const int r = wr_run(w, dir);
   wr_wipe(w, sizeof(*w));
   free(w);
   return r;
}
