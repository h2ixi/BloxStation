/* Local web relay for the PS5 browser: see web_relay.h.
 *
 * One thread per browser connection, one request per connection
 * ("Connection: close" both ways). Upstream: SOCKS5 CONNECT host:443 on the
 * in-title gateway, TLS 1.2 with BearSSL (Mozilla roots, sessions resumed per
 * host), "Accept-Encoding: identity". The whole response is read (chunked
 * bodies decoded), Roblox links in text bodies are pointed back at the relay,
 * and it goes to the browser with a Content-Length. */
#include "web_relay.h"

#include <dirent.h>
#include <sys/stat.h>

#include <bearssl.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "web_relay_tas.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WR_MAX_REQ (64 * 1024)
/* the Roblox app's web view: pages come in their in-app form (no site
 * header or side menu), as the Android app shows them */
#define WR_APP_UA                                                                                            \
   "Mozilla/5.0 (Linux; Android 13; PlayStation 5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 " \
   "Mobile Safari/537.36 Roblox/Android RobloxApp/2.746.1266 (GlobalDist; GooglePlayStore)"
#define WR_MAX_BODY (48u * 1024 * 1024)

static struct web_relay_hooks wr_hooks;
static unsigned short wr_port;
static char wr_socks_host[64] = "127.0.0.1";
static unsigned short wr_socks_port = 1080;
static char wr_origin[64]; /* "127.0.0.1:port" */
static unsigned wr_requests;

static void
wr_log(const char *fmt, ...)
{
   char line[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (wr_hooks.log)
      wr_hooks.log(line);
   else
      fprintf(stderr, "%s\n", line);
}

/* ---- growable buffer ---------------------------------------------------- */

struct buf {
   char *p;
   size_t n, cap;
};

static int
buf_add(struct buf *b, const void *d, size_t n)
{
   if (b->n + n + 1 > b->cap) {
      size_t c = b->cap ? b->cap : 4096;
      while (c < b->n + n + 1)
         c *= 2;
      char *q = realloc(b->p, c);
      if (!q)
         return -1;
      b->p = q, b->cap = c;
   }
   memcpy(b->p + b->n, d, n);
   b->n += n;
   b->p[b->n] = 0;
   return 0;
}

static int
buf_str(struct buf *b, const char *s)
{
   return buf_add(b, s, strlen(s));
}

static int
buf_fmt(struct buf *b, const char *fmt, ...)
{
   char tmp[2048];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
   va_end(ap);
   if (n < 0)
      return -1;
   return buf_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* ---- hosts and URLs ------------------------------------------------------ */

static int
ends_with(const char *h, size_t n, const char *suf)
{
   const size_t m = strlen(suf);
   if (n == m)
      return strncasecmp(h, suf, m) == 0;
   return n > m && h[n - m - 1] == '.' && strncasecmp(h + n - m, suf, m) == 0;
}

int
web_relay_host_ok(const char *host, size_t len)
{
   if (len == 0 || len > 120)
      return 0;
   return ends_with(host, len, "roblox.com") || ends_with(host, len, "rbxcdn.com") ||
          ends_with(host, len, "robloxlabs.com");
}

static int
host_char(char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

char *
web_relay_url(const char *url, char *out, size_t cap)
{
   const char *h = NULL;
   if (strncasecmp(url, "https://", 8) == 0)
      h = url + 8;
   else if (strncasecmp(url, "http://", 7) == 0)
      h = url + 7;
   size_t hl = 0;
   if (h)
      while (host_char(h[hl]))
         hl++;
   if (h && wr_port && web_relay_host_ok(h, hl))
      snprintf(out, cap, "http://%s/%s", wr_origin, h);
   else
      snprintf(out, cap, "%s", url);
   return out;
}

/* Point Roblox links in a text body back at the relay:
 *   https://H  http://H  //H   ->  http://127.0.0.1:P/H  (//127.0.0.1:P/H)
 * and the same with JSON-escaped slashes (https:\/\/H). */
static void
rewrite_links(const char *s, size_t n, struct buf *o)
{
   size_t i = 0, last = 0;
   while (i < n) {
      const char c = s[i];
      /* the app's own links: through the relay to the app ("/__app/...") */
      if (c == 'r' && i > 0 && (s[i - 1] == '"' || s[i - 1] == '\'' || s[i - 1] == '`' || s[i - 1] == '=')) {
         size_t m = 0;
         if (n - i >= 15 && memcmp(s + i, "robloxmobile://", 15) == 0)
            m = 15;
         else if (n - i >= 9 && memcmp(s + i, "roblox://", 9) == 0)
            m = 9;
         if (m) {
            buf_add(o, s + last, i - last);
            buf_fmt(o, "http://%s/__app/", wr_origin);
            buf_add(o, s + i, m);
            i = last = i + m;
            continue;
         }
      }
      if (c != 'h' && c != 'H' && c != '/' && c != '\\') {
         i++;
         continue;
      }
      size_t j = i;
      int scheme = 0;
      if (n - j >= 6 && strncasecmp(s + j, "https:", 6) == 0)
         j += 6, scheme = 1;
      else if (n - j >= 5 && strncasecmp(s + j, "http:", 5) == 0)
         j += 5, scheme = 1;
      int esc;
      if (n - j >= 2 && s[j] == '/' && s[j + 1] == '/')
         esc = 0, j += 2;
      else if (n - j >= 4 && memcmp(s + j, "\\/\\/", 4) == 0)
         esc = 1, j += 4;
      else {
         i++;
         continue;
      }
      /* "//" not after a scheme: only on its own (not wss://, not a/b//c) */
      if (!scheme && i > 0 && (s[i - 1] == ':' || host_char(s[i - 1]) || s[i - 1] == '/')) {
         i = j;
         continue;
      }
      size_t hl = 0;
      while (j + hl < n && host_char(s[j + hl]))
         hl++;
      while (hl && s[j + hl - 1] == '.')
         hl--;
      if (!web_relay_host_ok(s + j, hl)) {
         i = j;
         continue;
      }
      buf_add(o, s + last, i - last);
      if (scheme)
         buf_str(o, "http:");
      buf_str(o, esc ? "\\/\\/" : "//");
      buf_str(o, wr_origin);
      buf_str(o, esc ? "\\/" : "/");
      buf_add(o, s + j, hl);
      i = last = j + hl;
   }
   buf_add(o, s + last, n - last);
}

/* Fixes in Roblox's scripts: absolute links built from location.hostname
 * lose the relay's port ("/games/start" on Join went to 127.0.0.1:80) */
static const char *const wr_js_fix[][2] = {
   {"\"//\").concat(window.location.hostname)", "\"//\").concat(window.location.host)"},
   {"\"//\"+window.location.hostname", "\"//\"+window.location.host"},
};

/* memmem (not in the PS5's libc, like strcasestr) */
static const char *
wr_memmem(const char *h, size_t hn, const char *n, size_t nn)
{
   if (!nn || hn < nn)
      return NULL;
   for (size_t i = 0; i + nn <= hn; i++)
      if (h[i] == n[0] && memcmp(h + i, n, nn) == 0)
         return h + i;
   return NULL;
}

static void
js_fix(struct buf *b)
{
   for (unsigned k = 0; k < sizeof(wr_js_fix) / sizeof(wr_js_fix[0]); k++) {
      const char *from = wr_js_fix[k][0], *to = wr_js_fix[k][1];
      const size_t fl = strlen(from), tl = strlen(to);
      if (!b->n || !wr_memmem(b->p, b->n, from, fl))
         continue;
      struct buf o = {0};
      size_t i = 0;
      const char *hit;
      while ((hit = wr_memmem(b->p + i, b->n - i, from, fl))) {
         buf_add(&o, b->p + i, (size_t)(hit - (b->p + i)));
         buf_add(&o, to, tl);
         i = (size_t)(hit - b->p) + fl;
      }
      buf_add(&o, b->p + i, b->n - i);
      free(b->p);
      *b = o;
   }
}

/* For URLs built in scripts at run time: fetch / XHR / sendBeacon /
 * window.open with a Roblox host go through the relay as well. */
static const char wr_shim[] =
   "<script>(function(){var P=location.protocol+'//'+location.host+'/';"
   "var R=/^(?:https?:)?\\/\\/([a-z0-9.-]*(?:roblox\\.com|rbxcdn\\.com|robloxlabs\\.com))(?=[\\/?#]|$)/i;"
   "function r(u){if(typeof u!=='string')return u;var m=u.match(R);return m?P+m[1]+u.slice(m[0].length):u;}"
   "window.__ps5relay=r;var f=window.fetch;if(f)window.fetch=function(u,o){"
   "if(typeof u==='string')u=r(u);else if(u&&u.url){var n=r(u.url);if(n!==u.url)u=new Request(n,u);}"
   "return f.call(this,u,o);};var X=window.XMLHttpRequest&&XMLHttpRequest.prototype;"
   "if(X){var op=X.open;X.open=function(m,u){var a=[].slice.call(arguments);a[1]=r(String(u));return op.apply(this,a);};}"
   "if(navigator.sendBeacon){var sb=navigator.sendBeacon.bind(navigator);navigator.sendBeacon=function(u,d){return sb(r(String(u)),d);};}"
   "var wo=window.open;window.open=function(u){var a=[].slice.call(arguments);if(a.length)a[0]=r(String(u));return wo.apply(window,a);};"
   /* realtime (wss://realtime-signalr.roblox.com/...): ws:// through the relay */
   "var W=window.WebSocket;if(W){var WR=/^wss?:\\/\\/([a-z0-9.-]*(?:roblox\\.com|rbxcdn\\.com|robloxlabs\\.com))(?=[\\/?#]|$)/i;"
   "var N=function(u,p){u=String(u);var m=u.match(WR);if(m)u='ws://'+location.host+'/'+m[1]+u.slice(m[0].length);"
   "return p===undefined?new W(u):new W(u,p);};N.prototype=W.prototype;"
   "N.CONNECTING=0;N.OPEN=1;N.CLOSING=2;N.CLOSED=3;window.WebSocket=N;}"
   /* Circle = back one page, closing the browser on the first page: the
    * page sees the controller (Gamepad API) while the PS5 browser has it.
    * Pages opened in this browser are counted in sessionStorage. */
   "try{var S=sessionStorage,ty=0,pe=performance.getEntriesByType&&performance.getEntriesByType('navigation')[0];"
   "ty=pe?({navigate:0,reload:1,back_forward:2})[pe.type]||0:(performance.navigation||{}).type||0;"
   "var d=+(S.ps5d||0);d=history.length<2?1:ty==0?d+1:ty==2?Math.max(1,d-1):Math.max(1,d);S.ps5d=d;"
   /* views a page opens inside itself (pushState, e.g. a message in Messages)
    * count as pages too; going back from one (popstate) uncounts it */
   "var PS=history.pushState;history.pushState=function(){S.ps5d=+S.ps5d+1;return PS.apply(history,arguments);};"
   "window.addEventListener('popstate',function(){S.ps5d=Math.max(1,+S.ps5d-1);});"
   "var B=0,bk=function(){if(B)return;B=1;setTimeout(function(){B=0;},600);"
   "if(+S.ps5d>1)history.back();else{S.ps5d=0;location.href='/__close';}};"
   "document.addEventListener('keydown',function(e){var t=e.target&&e.target.tagName,ed=t=='INPUT'||t=='TEXTAREA'||(e.target&&e.target.isContentEditable);"
   "if(e.keyCode==27||(e.keyCode==8&&!ed)){e.preventDefault();bk();}},true);"
   "var gp=0;if(navigator.getGamepads)setInterval(function(){var g=navigator.getGamepads()||[],p=0;"
   "for(var i=0;i<g.length;i++)if(g[i]&&g[i].buttons[1]&&g[i].buttons[1].pressed)p=1;if(p&&!gp)bk();gp=p;},50);"
   "}catch(e){}"
   "})();</script>"
   /* the TV is far away: in-app pages at a size like the app's own */
   "<style>html{zoom:1.5}</style>";

/* ---- sockets ------------------------------------------------------------- */

static int
send_all(int fd, const void *d, size_t n)
{
   const char *p = d;
   while (n) {
      ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
      if (w < 0 && errno == EINTR)
         continue;
      if (w <= 0)
         return -1;
      p += w, n -= (size_t)w;
   }
   return 0;
}

static int
recv_all(int fd, void *d, size_t n)
{
   char *p = d;
   while (n) {
      ssize_t r = recv(fd, p, n, 0);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0)
         return -1;
      p += r, n -= (size_t)r;
   }
   return 0;
}

static void
set_timeouts(int fd, int secs)
{
   struct timeval tv = {secs, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/* SOCKS5 CONNECT host:port by name: the connected fd, or -1 */
static int
socks_connect(const char *host, unsigned port)
{
   const size_t hl = strlen(host);
   if (hl > 255)
      return -1;
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   set_timeouts(fd, 30);
   struct sockaddr_in a;
   memset(&a, 0, sizeof(a));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
   a.sin_len = sizeof(a);
#endif
   a.sin_family = AF_INET;
   a.sin_port = htons(wr_socks_port);
   inet_pton(AF_INET, wr_socks_host, &a.sin_addr);
   unsigned char m[300];
   if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
      goto fail;
   if (send_all(fd, "\x05\x01\x00", 3) || recv_all(fd, m, 2) || m[0] != 5 || m[1] != 0)
      goto fail;
   size_t k = 0;
   m[k++] = 5, m[k++] = 1, m[k++] = 0, m[k++] = 3, m[k++] = (unsigned char)hl;
   memcpy(m + k, host, hl), k += hl;
   m[k++] = (unsigned char)(port >> 8), m[k++] = (unsigned char)port;
   if (send_all(fd, m, k) || recv_all(fd, m, 4) || m[1] != 0)
      goto fail;
   size_t rest = m[3] == 1 ? 6 : m[3] == 4 ? 18 : 0;
   if (m[3] == 3) {
      if (recv_all(fd, m, 1))
         goto fail;
      rest = (size_t)m[0] + 2;
   }
   if (!rest || recv_all(fd, m, rest))
      goto fail;
   return fd;
fail:
   close(fd);
   return -1;
}

/* ---- TLS ----------------------------------------------------------------- */

static struct {
   char host[128];
   br_ssl_session_parameters sp;
} wr_sess[32];
static unsigned wr_sess_next;
static pthread_mutex_t wr_sess_lock = PTHREAD_MUTEX_INITIALIZER;

static int
tls_read(void *ctx, unsigned char *b, size_t n)
{
   for (;;) {
      ssize_t r = recv(*(int *)ctx, b, n, 0);
      if (r < 0 && errno == EINTR)
         continue;
      return r <= 0 ? -1 : (int)r;
   }
}

static int
tls_write(void *ctx, const unsigned char *b, size_t n)
{
   for (;;) {
      ssize_t w = send(*(int *)ctx, b, n, MSG_NOSIGNAL);
      if (w < 0 && errno == EINTR)
         continue;
      return w <= 0 ? -1 : (int)w;
   }
}

struct up {
   int fd;
   br_ssl_client_context sc;
   br_x509_minimal_context xc;
   br_sslio_context io;
   unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static int
up_open(struct up *u, const char *host)
{
   u->fd = socks_connect(host, 443);
   if (u->fd < 0)
      return -1;
   br_ssl_client_init_full(&u->sc, &u->xc, TAs, TAs_NUM);
   br_ssl_engine_set_buffer(&u->sc.eng, u->iobuf, sizeof(u->iobuf), 1);
   const time_t t = time(NULL);
   br_x509_minimal_set_time(&u->xc, (uint32_t)(t / 86400 + 719528), (uint32_t)(t % 86400));
   unsigned char seed[32];
   if (wr_hooks.random)
      wr_hooks.random(seed, sizeof(seed));
   else {
      for (size_t i = 0; i < sizeof(seed); i++)
         seed[i] = (unsigned char)rand();
   }
   br_ssl_engine_inject_entropy(&u->sc.eng, seed, sizeof(seed));
   int resume = 0;
   pthread_mutex_lock(&wr_sess_lock);
   for (unsigned i = 0; i < 32; i++)
      if (wr_sess[i].host[0] && strcasecmp(wr_sess[i].host, host) == 0) {
         br_ssl_engine_set_session_parameters(&u->sc.eng, &wr_sess[i].sp);
         resume = 1;
         break;
      }
   pthread_mutex_unlock(&wr_sess_lock);
   if (!br_ssl_client_reset(&u->sc, host, resume)) {
      close(u->fd);
      return -1;
   }
   br_sslio_init(&u->io, &u->sc.eng, tls_read, &u->fd, tls_write, &u->fd);
   return 0;
}

static void
up_keep_session(struct up *u, const char *host)
{
   br_ssl_session_parameters sp;
   br_ssl_engine_get_session_parameters(&u->sc.eng, &sp);
   if (!sp.session_id_len)
      return;
   pthread_mutex_lock(&wr_sess_lock);
   unsigned slot = 32;
   for (unsigned i = 0; i < 32; i++)
      if (strcasecmp(wr_sess[i].host, host) == 0)
         slot = i;
   if (slot == 32)
      slot = wr_sess_next++ % 32;
   snprintf(wr_sess[slot].host, sizeof(wr_sess[slot].host), "%s", host);
   wr_sess[slot].sp = sp;
   pthread_mutex_unlock(&wr_sess_lock);
}

/* Idle upstream connections, reused for the next request to the same host:
 * a fresh one costs a SOCKS connect, a TCP and a TLS handshake through the
 * tunnel (about 1 s); a reused one only the request itself. */
static int read_response(struct up *u, const char *method, struct buf *hdr, struct buf *body, int *status,
                         int *reusable);
#define WR_POOL 24
static struct {
   char host[128];
   struct up *u;
   time_t at;
} wr_pool[WR_POOL];
static pthread_mutex_t wr_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void
up_free(struct up *u)
{
   if (u) {
      close(u->fd);
      free(u);
   }
}

static struct up *
pool_take(const char *host)
{
   struct up *u = NULL, *stale[WR_POOL];
   unsigned ns = 0;
   const time_t now = time(NULL);
   pthread_mutex_lock(&wr_pool_lock);
   for (unsigned i = 0; i < WR_POOL; i++) {
      if (!wr_pool[i].u)
         continue;
      if (now - wr_pool[i].at > 25) { /* servers drop idle ones after ~30 s */
         stale[ns++] = wr_pool[i].u;
         wr_pool[i].u = NULL;
      } else if (!u && strcasecmp(wr_pool[i].host, host) == 0) {
         /* an idle connection must have nothing to read: data (a stray
          * response) or EOF means it cannot carry the next request */
         struct pollfd pf = {wr_pool[i].u->fd, POLLIN, 0};
         const int busy = poll(&pf, 1, 0) != 0 ||
                          (br_ssl_engine_current_state(&wr_pool[i].u->sc.eng) & BR_SSL_RECVAPP);
         if (busy)
            stale[ns++] = wr_pool[i].u;
         else
            u = wr_pool[i].u;
         wr_pool[i].u = NULL;
      }
   }
   pthread_mutex_unlock(&wr_pool_lock);
   for (unsigned i = 0; i < ns; i++)
      up_free(stale[i]);
   return u;
}

static void
pool_put(const char *host, struct up *u)
{
   struct up *old = NULL;
   pthread_mutex_lock(&wr_pool_lock);
   unsigned slot = 0;
   for (unsigned i = 0; i < WR_POOL; i++) {
      if (!wr_pool[i].u) {
         slot = i;
         break;
      }
      if (wr_pool[i].at < wr_pool[slot].at)
         slot = i;
   }
   old = wr_pool[slot].u;
   snprintf(wr_pool[slot].host, sizeof(wr_pool[slot].host), "%s", host);
   wr_pool[slot].u = u;
   wr_pool[slot].at = time(NULL);
   pthread_mutex_unlock(&wr_pool_lock);
   up_free(old);
}

/* one request upstream, on a pooled connection when there is one (a dead
 * pooled one is retried once on a fresh connection): 0, or -1 */
static int
upstream(const char *host, const char *method, const struct buf *upreq, struct buf *hdr, struct buf *body,
         int *status, int *tls_err)
{
   for (int attempt = 0; attempt < 2; attempt++) {
      struct up *u = attempt == 0 ? pool_take(host) : NULL;
      const int pooled = u != NULL;
      if (!u) {
         u = malloc(sizeof(*u));
         if (!u || up_open(u, host) != 0) {
            free(u);
            *tls_err = -1;
            return -1;
         }
      }
      hdr->n = body->n = 0;
      int reusable = 0;
      if (br_sslio_write_all(&u->io, upreq->p, upreq->n) != 0 || br_sslio_flush(&u->io) != 0 ||
          read_response(u, method, hdr, body, status, &reusable) != 0) {
         *tls_err = br_ssl_engine_last_error(&u->sc.eng);
         up_keep_session(u, host);
         up_free(u);
         if (pooled)
            continue;
         return -1;
      }
      up_keep_session(u, host);
      if (reusable)
         pool_put(host, u);
      else
         up_free(u);
      return 0;
   }
   return -1;
}

/* ---- disk cache for Roblox's static files ---------------------------------
 * rbxcdn.com serves content-addressed files (hashes in the names) that never
 * change: the finished response (headers + rewritten body) is kept on disk. */
static char wr_cache_dir[256];

/* case-insensitive strstr (the PS5's libc has no strcasestr) */
static const char *
ci_find(const char *h, const char *n)
{
   const size_t nl = strlen(n);
   for (; *h; h++)
      if (strncasecmp(h, n, nl) == 0)
         return h;
   return NULL;
}

static int
cache_ok_request(const char *method, const char *host)
{
   return wr_cache_dir[0] && strcasecmp(method, "GET") == 0 && ends_with(host, strlen(host), "rbxcdn.com");
}

static void
cache_path(const char *host, const char *path, char *out, size_t cap)
{
   uint64_t h = 1469598103934665603ull;
   for (const char *p = host; *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ull;
   h = (h ^ '/') * 1099511628211ull;
   for (const char *p = path; *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ull;
   snprintf(out, cap, "%s/%016llx", wr_cache_dir, (unsigned long long)h);
}

static int
cache_send(int cfd, const char *host, const char *path)
{
   char fn[300];
   cache_path(host, path, fn, sizeof(fn));
   FILE *f = fopen(fn, "rb");
   if (!f)
      return 0;
   char tmp[65536];
   size_t n;
   int ok = 1;
   while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
      if (send_all(cfd, tmp, n) != 0) {
         ok = 1; /* the browser went away: still served from the cache */
         break;
      }
   fclose(f);
   return ok;
}

static void
cache_store(const char *host, const char *path, const struct buf *h, const struct buf *b)
{
   char fn[300], tmpn[320];
   cache_path(host, path, fn, sizeof(fn));
   snprintf(tmpn, sizeof(tmpn), "%s.%lx.tmp", fn, (unsigned long)pthread_self());
   FILE *f = fopen(tmpn, "wb");
   if (!f)
      return;
   const int ok = fwrite(h->p, 1, h->n, f) == h->n && (!b->n || fwrite(b->p, 1, b->n, f) == b->n);
   if (fclose(f) != 0 || !ok || rename(tmpn, fn) != 0)
      unlink(tmpn);
}

/* ---- HTTP ---------------------------------------------------------------- */

/* the value of header name in a header block (lines after the first), or NULL */
static const char *
hdr_get(const char *h, const char *name, size_t *len)
{
   const size_t nl = strlen(name);
   for (const char *p = strstr(h, "\r\n"); p && p[2] && !(p[2] == '\r' && p[3] == '\n'); p = strstr(p + 2, "\r\n")) {
      const char *l = p + 2;
      if (strncasecmp(l, name, nl) == 0 && l[nl] == ':') {
         const char *v = l + nl + 1;
         while (*v == ' ' || *v == '\t')
            v++;
         const char *e = strstr(v, "\r\n");
         *len = e ? (size_t)(e - v) : strlen(v);
         return v;
      }
   }
   return NULL;
}

static int
hdr_is(const char *line, const char *name)
{
   const size_t n = strlen(name);
   return strncasecmp(line, name, n) == 0 && line[n] == ':';
}

static void
reply_simple(int fd, int code, const char *reason, const char *extra_hdr, const char *body)
{
   struct buf b = {0};
   buf_fmt(&b, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
               "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
           code, reason, strlen(body), extra_hdr ? extra_hdr : "");
   buf_str(&b, body);
   send_all(fd, b.p, b.n);
   free(b.p);
}

/* the relayed host in "http://127.0.0.1:P/<host>/..." (a Referer), or 0 */
static size_t
referer_host(const char *v, size_t vl, const char **host)
{
   char pre[96];
   snprintf(pre, sizeof(pre), "http://%s/", wr_origin);
   const size_t pl = strlen(pre);
   if (vl <= pl || strncmp(v, pre, pl) != 0)
      return 0;
   size_t hl = 0;
   while (pl + hl < vl && host_char(v[pl + hl]))
      hl++;
   if (!web_relay_host_ok(v + pl, hl))
      return 0;
   *host = v + pl;
   return hl;
}

/* cookie names already in "a=1; b=2" */
static int
cookie_has(const char *list, const char *name, size_t nl)
{
   for (const char *p = list; *p;) {
      while (*p == ' ' || *p == ';')
         p++;
      if (strncmp(p, name, nl) == 0 && p[nl] == '=')
         return 1;
      p = strchr(p, ';');
      if (!p)
         break;
   }
   return 0;
}

/* Read a whole upstream response into hdr (status line + headers, with the
 * final CRLFCRLF) and body (decoded). 0, or -1. */
static int
read_response(struct up *u, const char *method, struct buf *hdr, struct buf *body, int *status, int *reusable)
{
   *reusable = 0;
   struct buf raw = {0};
   char tmp[16384];
   char *eoh = NULL;
   int eof = 0;
   while (!eoh) {
      int r = br_sslio_read(&u->io, tmp, sizeof(tmp));
      if (r <= 0) {
         free(raw.p);
         return -1;
      }
      buf_add(&raw, tmp, (size_t)r);
      eoh = strstr(raw.p, "\r\n\r\n");
      if (!eoh && raw.n > WR_MAX_REQ) {
         free(raw.p);
         return -1;
      }
   }
   const size_t hn = (size_t)(eoh - raw.p) + 4;
   buf_add(hdr, raw.p, hn);
   *status = 0;
   sscanf(hdr->p, "HTTP/%*s %d", status);
   size_t vl;
   const char *v;
   const int no_body = strcasecmp(method, "HEAD") == 0 || *status == 204 || *status == 304 ||
                       (*status >= 100 && *status < 200);
   int chunked = (v = hdr_get(hdr->p, "Transfer-Encoding", &vl)) && vl >= 7 && strncasecmp(v + vl - 7, "chunked", 7) == 0;
   long long clen = -1;
   if ((v = hdr_get(hdr->p, "Content-Length", &vl)))
      clen = atoll(v);
   struct buf in = {0};
   buf_add(&in, raw.p + hn, raw.n - hn);
   free(raw.p);
   const char *cv;
   size_t cvl;
   const int conn_close = (cv = hdr_get(hdr->p, "Connection", &cvl)) && cvl >= 5 && strncasecmp(cv, "close", 5) == 0;
   if (no_body) {
      *reusable = !conn_close && in.n == 0;
      free(in.p);
      return 0;
   }
   /* read until the body is complete (or EOF) */
   size_t pos = 0; /* chunked: parse position in in */
   for (;;) {
      if (!chunked && clen >= 0 && (long long)in.n >= clen) {
         buf_add(body, in.p, (size_t)clen);
         *reusable = !conn_close && (long long)in.n == clen;
         break;
      }
      if (chunked) {
         int done = 0;
         for (;;) {
            char *crlf = in.n > pos ? strstr(in.p + pos, "\r\n") : NULL;
            if (!crlf)
               break;
            const unsigned long sz = strtoul(in.p + pos, NULL, 16);
            const size_t start = (size_t)(crlf - in.p) + 2;
            if (sz == 0) {
               done = 1;
               /* no trailers and nothing after: the connection can be reused */
               *reusable = !conn_close && in.n == start + 2 && memcmp(in.p + start, "\r\n", 2) == 0;
               break;
            }
            if (in.n < start + sz + 2)
               break;
            buf_add(body, in.p + start, sz);
            pos = start + sz + 2;
         }
         if (done)
            break;
      }
      if (eof)
         break;
      if (in.n > WR_MAX_BODY) {
         free(in.p);
         return -1;
      }
      int r = br_sslio_read(&u->io, tmp, sizeof(tmp));
      if (r <= 0) {
         eof = 1;
         if (!chunked && clen < 0) {
            buf_add(body, in.p, in.n);
            break;
         }
         if (!chunked && clen >= 0) { /* short body: give what came */
            buf_add(body, in.p, in.n);
            break;
         }
         continue; /* chunked: parse what is there, then stop */
      }
      buf_add(&in, tmp, (size_t)r);
   }
   free(in.p);
   return 0;
}

/* ---- WebSocket -----------------------------------------------------------
 * After the upstream answered 101, bytes go both ways unchanged until either
 * side closes. One thread drives the TLS engine directly (BearSSL contexts
 * are not thread-safe, so no reader/writer thread pair). */
static unsigned wr_ws_open;

static void
ws_pump(int cfd, struct up *u, const char *host)
{
   br_ssl_engine_context *eng = &u->sc.eng;
   unsigned long long up_bytes = 0, down_bytes = 0;
   const unsigned n_open = __atomic_add_fetch(&wr_ws_open, 1, __ATOMIC_RELAXED);
   wr_log("web relay: websocket to %s open (%u open)", host, n_open);
   const char *why = "closed";
   for (;;) {
      const unsigned st = br_ssl_engine_current_state(eng);
      if (st & BR_SSL_CLOSED) {
         why = "upstream TLS closed";
         break;
      }
      size_t len;
      if (st & BR_SSL_SENDREC) {
         unsigned char *b = br_ssl_engine_sendrec_buf(eng, &len);
         const int w = tls_write(&u->fd, b, len);
         if (w < 0) {
            why = "upstream send failed";
            break;
         }
         br_ssl_engine_sendrec_ack(eng, (size_t)w);
         continue;
      }
      if (st & BR_SSL_RECVAPP) {
         unsigned char *b = br_ssl_engine_recvapp_buf(eng, &len);
         if (send_all(cfd, b, len) != 0) {
            why = "browser went away";
            break;
         }
         down_bytes += len;
         br_ssl_engine_recvapp_ack(eng, len);
         continue;
      }
      struct pollfd pf[2] = {{cfd, (st & BR_SSL_SENDAPP) ? POLLIN : 0, 0},
                             {u->fd, (st & BR_SSL_RECVREC) ? POLLIN : 0, 0}};
      const int pr = poll(pf, 2, 300 * 1000);
      if (pr < 0 && errno == EINTR)
         continue;
      if (pr <= 0) {
         why = pr == 0 ? "idle 5 min" : "poll failed";
         break;
      }
      if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
         unsigned char *b = br_ssl_engine_sendapp_buf(eng, &len);
         const ssize_t r = b ? recv(cfd, b, len, 0) : 0;
         if (r < 0 && errno == EINTR)
            continue;
         if (r <= 0) {
            why = "browser closed";
            break;
         }
         br_ssl_engine_sendapp_ack(eng, (size_t)r);
         br_ssl_engine_flush(eng, 0);
         up_bytes += (unsigned long long)r;
      }
      if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
         unsigned char *b = br_ssl_engine_recvrec_buf(eng, &len);
         const int r = tls_read(&u->fd, b, len);
         if (r < 0) {
            why = "upstream closed";
            break;
         }
         br_ssl_engine_recvrec_ack(eng, (size_t)r);
      }
   }
   __atomic_sub_fetch(&wr_ws_open, 1, __ATOMIC_RELAXED);
   wr_log("web relay: websocket to %s ended (%s; %llu bytes up, %llu down)", host, why, up_bytes, down_bytes);
}

/* the upstream handshake for a WebSocket upgrade, then the pump */
static void
ws_relay(int cfd, const char *host, const struct buf *upreq)
{
   struct up *u = malloc(sizeof(*u));
   if (!u || up_open(u, host) != 0) {
      free(u);
      wr_log("web relay: websocket: cannot reach %s through the tunnel", host);
      reply_simple(cfd, 502, "Bad Gateway", NULL, "");
      return;
   }
   /* no read timeout while the socket idles between messages */
   struct timeval tv = {0, 0};
   setsockopt(u->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   struct buf raw = {0};
   char tmp[4096];
   char *eoh = NULL;
   if (br_sslio_write_all(&u->io, upreq->p, upreq->n) != 0 || br_sslio_flush(&u->io) != 0)
      goto fail;
   while (!eoh) {
      const int r = br_sslio_read(&u->io, tmp, sizeof(tmp));
      if (r <= 0 || raw.n > WR_MAX_REQ)
         goto fail;
      buf_add(&raw, tmp, (size_t)r);
      eoh = strstr(raw.p, "\r\n\r\n");
   }
   int status = 0;
   sscanf(raw.p, "HTTP/%*s %d", &status);
   const size_t hn = (size_t)(eoh - raw.p) + 4;
   /* the answer goes to the browser as it came (a refusal included), minus
    * cookies and the headers the browser must not see from another origin */
   struct buf out = {0};
   const char *sl_end = strstr(raw.p, "\r\n");
   buf_add(&out, raw.p, (size_t)(sl_end - raw.p) + 2);
   for (const char *p = sl_end; p && p + 2 < raw.p + hn - 2; p = strstr(p + 2, "\r\n")) {
      const char *l = p + 2;
      const char *e = strstr(l, "\r\n");
      if (!e || l == e)
         break;
      if (hdr_is(l, "Set-Cookie") || hdr_is(l, "Strict-Transport-Security") || hdr_is(l, "Alt-Svc") ||
          hdr_is(l, "Content-Security-Policy"))
         continue;
      buf_add(&out, l, (size_t)(e - l));
      buf_str(&out, "\r\n");
   }
   buf_str(&out, "\r\n");
   if (raw.n > hn)
      buf_add(&out, raw.p + hn, raw.n - hn);
   const int sent = send_all(cfd, out.p, out.n);
   free(out.p);
   if (status != 101) {
      wr_log("web relay: websocket to %s refused (%d)", host, status);
   } else if (sent == 0) {
      ws_pump(cfd, u, host);
   }
   free(raw.p);
   up_free(u);
   return;
fail:
   wr_log("web relay: websocket to %s: handshake failed (TLS error %d)", host, br_ssl_engine_last_error(&u->sc.eng));
   free(raw.p);
   up_free(u);
   reply_simple(cfd, 502, "Bad Gateway", NULL, "");
}

static int
is_text(const char *ct, size_t n)
{
   static const char *const t[] = {"text/", "application/javascript", "application/x-javascript",
                                    "application/json", "application/xml", "application/manifest",
                                    "image/svg", "application/ecmascript"};
   for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
      if (n >= strlen(t[i]) && strncasecmp(ct, t[i], strlen(t[i])) == 0)
         return 1;
   return 0;
}

static void
handle(int cfd)
{
   set_timeouts(cfd, 60);
   struct buf req = {0}, hdr = {0}, body = {0}, out = {0}, upreq = {0};
   char tmp[8192];
   char *eoh = NULL;
   while (!eoh) {
      ssize_t r = recv(cfd, tmp, sizeof(tmp), 0);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0 || req.n > WR_MAX_REQ)
         goto done;
      buf_add(&req, tmp, (size_t)r);
      eoh = strstr(req.p, "\r\n\r\n");
   }
   const size_t hn = (size_t)(eoh - req.p) + 4;
   char method[16], target[4096];
   if (sscanf(req.p, "%15s %4095s", method, target) != 2 || target[0] != '/')
      goto done;
   size_t vl;
   const char *v;
   long long clen = 0;
   if ((v = hdr_get(req.p, "Content-Length", &vl)))
      clen = atoll(v);
   if (clen < 0 || clen > (long long)WR_MAX_BODY)
      goto done;
   while (req.n < hn + (size_t)clen) {
      ssize_t r = recv(cfd, tmp, sizeof(tmp), 0);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0)
         goto done;
      buf_add(&req, tmp, (size_t)r);
   }

   if (strcmp(target, "/__close") == 0) { /* Circle on the first page */
      wr_log("web relay: page asked to close the browser");
      if (wr_hooks.app_link)
         wr_hooks.app_link("ps5://close");
      reply_simple(cfd, 200, "OK", NULL, "<html><body></body></html>");
      goto done;
   }
   if (strncmp(target, "/__app/", 7) == 0) {
      wr_log("web relay: page link for the app: %.200s", target + 7);
      if (wr_hooks.app_link)
         wr_hooks.app_link(target + 7);
      reply_simple(cfd, 200, "OK", NULL,
                   "<html><body style='font:28px sans-serif;text-align:center;padding-top:200px'>"
                   "Opening in Roblox...</body></html>");
      goto done;
   }
   /* the page's Join built "/games/start?..." on the bare host (port 80,
    * the second listener) */
   if (strncmp(target, "/games/start?", 13) == 0) {
      char link[4200];
      snprintf(link, sizeof(link), "roblox://experiences/start?%s", target + 13);
      wr_log("web relay: game start for the app: %.200s", link);
      if (wr_hooks.app_link)
         wr_hooks.app_link(link);
      reply_simple(cfd, 200, "OK", NULL,
                   "<html><body style='font:28px sans-serif;text-align:center;padding-top:200px'>"
                   "Joining...</body></html>");
      goto done;
   }
   /* /<host>/<path> */
   char host[128];
   size_t hl = 0;
   while (host_char(target[1 + hl]))
      hl++;
   const char *path = target + 1 + hl;
   if (!web_relay_host_ok(target + 1, hl) || (*path && *path != '/' && *path != '?')) {
      /* a root-relative URL from a relayed page: send it under the page's host */
      const char *rh = NULL;
      size_t rl = 0;
      if ((v = hdr_get(req.p, "Referer", &vl)))
         rl = referer_host(v, vl, &rh);
      if (!rl)
         rh = "www.roblox.com", rl = 14;
      char loc[4300];
      snprintf(loc, sizeof(loc), "Location: /%.*s%s\r\n", (int)rl, rh, target);
      reply_simple(cfd, 307, "Temporary Redirect", loc, "");
      goto done;
   }
   snprintf(host, sizeof(host), "%.*s", (int)hl, target + 1);
   if (!*path)
      path = "/";

   /* the website's game start page: a join, for the app */
   if (strncmp(path, "/games/start?", 13) == 0 && ends_with(host, strlen(host), "roblox.com")) {
      char link[4200];
      snprintf(link, sizeof(link), "roblox://experiences/start?%s", path + 13);
      wr_log("web relay: game start for the app: %.200s", link);
      if (wr_hooks.app_link)
         wr_hooks.app_link(link);
      reply_simple(cfd, 200, "OK", NULL,
                   "<html><body style='font:28px sans-serif;text-align:center;padding-top:200px'>"
                   "Joining...</body></html>");
      goto done;
   }
   /* keep the app signed in: the website must not end the session */
   if (strcasecmp(host, "auth.roblox.com") == 0 && strstr(path, "/logout")) {
      wr_log("web relay: blocked %s %s%s (would sign the app out)", method, host, path);
      reply_simple(cfd, 403, "Forbidden", NULL, "{\"errors\":[{\"code\":0,\"message\":\"Sign out from the app instead.\"}]}");
      goto done;
   }

   /* the request upstream */
   const char *page_host = NULL;
   size_t page_hl = 0;
   if ((v = hdr_get(req.p, "Referer", &vl)))
      page_hl = referer_host(v, vl, &page_host);
   buf_fmt(&upreq, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path[0] == '?' ? "/" : path, host);
   if (path[0] == '?') { /* "/host?x" -> "/?x" */
      upreq.n = 0;
      buf_fmt(&upreq, "%s /%s HTTP/1.1\r\nHost: %s\r\n", method, path, host);
   }
   char inject[8192] = "";
   if (wr_hooks.cookies && ends_with(host, strlen(host), "roblox.com"))
      wr_hooks.cookies(host, inject, sizeof(inject));
   struct buf ck = {0};
   buf_str(&ck, inject);
   for (const char *p = strstr(req.p, "\r\n"); p && p + 2 < req.p + hn - 2; p = strstr(p + 2, "\r\n")) {
      const char *l = p + 2;
      const char *e = strstr(l, "\r\n");
      if (!e || l == e)
         break;
      const size_t ll = (size_t)(e - l);
      if (hdr_is(l, "Host") || hdr_is(l, "Connection") || hdr_is(l, "Keep-Alive") || hdr_is(l, "Proxy-Connection") ||
          hdr_is(l, "Proxy-Authorization") || hdr_is(l, "Accept-Encoding") || hdr_is(l, "Upgrade") ||
          hdr_is(l, "TE") || hdr_is(l, "Transfer-Encoding") || hdr_is(l, "Content-Length"))
         continue;
      if (hdr_is(l, "Cookie")) { /* the browser's own cookies, after ours */
         const char *c = l + 7;
         while (c < e) {
            while (c < e && (*c == ' ' || *c == ';'))
               c++;
            const char *ce = memchr(c, ';', (size_t)(e - c));
            if (!ce)
               ce = e;
            const char *eq = memchr(c, '=', (size_t)(ce - c));
            if (eq && !cookie_has(inject, c, (size_t)(eq - c))) {
               if (ck.n)
                  buf_str(&ck, "; ");
               buf_add(&ck, c, (size_t)(ce - c));
            }
            c = ce;
         }
         continue;
      }
      if (hdr_is(l, "User-Agent")) {
         buf_fmt(&upreq, "User-Agent: %s\r\n", WR_APP_UA);
         continue;
      }
      if (hdr_is(l, "Origin")) {
         buf_fmt(&upreq, "Origin: https://%.*s\r\n", page_hl ? (int)page_hl : 14, page_hl ? page_host : "www.roblox.com");
         continue;
      }
      if (hdr_is(l, "Referer")) {
         char pre[96];
         snprintf(pre, sizeof(pre), "http://%s/", wr_origin);
         const char *rv = l + 8;
         while (*rv == ' ')
            rv++;
         if (page_hl && strncmp(rv, pre, strlen(pre)) == 0) {
            buf_str(&upreq, "Referer: https://");
            buf_add(&upreq, rv + strlen(pre), (size_t)(e - rv) - strlen(pre));
            buf_str(&upreq, "\r\n");
         }
         continue;
      }
      buf_add(&upreq, l, ll);
      buf_str(&upreq, "\r\n");
   }
   if (ck.n)
      buf_fmt(&upreq, "Cookie: %s\r\n", ck.p);
   free(ck.p);
   if (clen > 0 || strcasecmp(method, "POST") == 0 || strcasecmp(method, "PUT") == 0 ||
       strcasecmp(method, "PATCH") == 0)
      buf_fmt(&upreq, "Content-Length: %lld\r\n", clen);
   /* a WebSocket (realtime notifications, chat): its own upstream connection */
   const char *uv = hdr_get(req.p, "Upgrade", &vl);
   if (uv && vl == 9 && strncasecmp(uv, "websocket", 9) == 0 && strcasecmp(method, "GET") == 0) {
      buf_str(&upreq, "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
      ws_relay(cfd, host, &upreq);
      goto done;
   }
   buf_str(&upreq, "Accept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n");
   if (clen > 0)
      buf_add(&upreq, req.p + hn, (size_t)clen);

   struct timeval t0, t1;
   gettimeofday(&t0, NULL);
   if (cache_ok_request(method, host) && cache_send(cfd, host, path)) {
      const unsigned k = __atomic_add_fetch(&wr_requests, 1, __ATOMIC_RELAXED);
      if (k <= 300 || k % 50 == 0)
         wr_log("web relay: #%u %s%.100s from the cache", k, host, path);
      goto done;
   }
   int status = 0, tls_err = 0;
   if (upstream(host, method, &upreq, &hdr, &body, &status, &tls_err) != 0) {
      if (tls_err == -1) {
         wr_log("web relay: cannot reach %s through the tunnel", host);
         reply_simple(cfd, 502, "Bad Gateway", NULL,
                      "<html><body style='font:20px sans-serif;padding:40px'>Cannot reach Roblox through the tunnel "
                      "right now. Press Triangle to reload.</body></html>");
      } else {
         wr_log("web relay: %s %s%.80s failed (TLS error %d)", method, host, path, tls_err);
         reply_simple(cfd, 502, "Bad Gateway", NULL,
                      "<html><body style='font:20px sans-serif;padding:40px'>Roblox did not answer. Press Triangle "
                      "to reload.</body></html>");
      }
      goto done;
   }

   /* the response to the browser */
   const char *ct = hdr_get(hdr.p, "Content-Type", &vl);
   const size_t ctl = ct ? vl : 0;
   size_t cel = 0;
   const char *ce = hdr_get(hdr.p, "Content-Encoding", &cel);
   const int text = ct && is_text(ct, ctl) && (!ce || (cel == 8 && strncasecmp(ce, "identity", 8) == 0));
   const int html = text && ctl >= 9 && strncasecmp(ct, "text/html", 9) == 0;
   const char *sl_end = strstr(hdr.p, "\r\n");
   buf_add(&out, hdr.p, (size_t)(sl_end - hdr.p) + 2);
   for (const char *p = sl_end; p && p[2] && !(p[2] == '\r' && p[3] == '\n'); p = strstr(p + 2, "\r\n")) {
      const char *l = p + 2;
      const char *e = strstr(l, "\r\n");
      if (!e)
         break;
      const size_t ll = (size_t)(e - l);
      if (hdr_is(l, "Connection") || hdr_is(l, "Keep-Alive") || hdr_is(l, "Transfer-Encoding") ||
          hdr_is(l, "Content-Length") || hdr_is(l, "Content-Security-Policy") ||
          hdr_is(l, "Content-Security-Policy-Report-Only") || hdr_is(l, "Strict-Transport-Security") ||
          hdr_is(l, "Alt-Svc") || hdr_is(l, "Referrer-Policy") || hdr_is(l, "Report-To") || hdr_is(l, "NEL") ||
          hdr_is(l, "Public-Key-Pins") || hdr_is(l, "Cross-Origin-Opener-Policy") ||
          hdr_is(l, "Cross-Origin-Embedder-Policy") || hdr_is(l, "Cross-Origin-Resource-Policy"))
         continue;
      if (hdr_is(l, "Location")) {
         buf_str(&out, "Location: ");
         const char *lv = l + 9;
         while (*lv == ' ')
            lv++;
         if (lv[0] == '/' && lv[1] != '/') /* root-relative: stay on this host */
            buf_fmt(&out, "/%s", host);
         rewrite_links(lv, (size_t)(e - lv), &out);
         buf_str(&out, "\r\n");
         continue;
      }
      if (hdr_is(l, "Set-Cookie")) {
         const char *sv = l + 11;
         while (*sv == ' ')
            sv++;
         if (strncmp(sv, ".ROBLOSECURITY=", 15) == 0) {
            wr_log("web relay: %s sent a new sign-in cookie (kept out of the browser)", host);
            continue;
         }
         /* host-only, http, no SameSite rules: the browser sees one origin */
         buf_str(&out, "Set-Cookie: ");
         const char *a = sv;
         int first = 1;
         while (a < e) {
            const char *ae = memchr(a, ';', (size_t)(e - a));
            if (!ae)
               ae = e;
            const char *s = a;
            while (s < ae && *s == ' ')
               s++;
            const size_t al = (size_t)(ae - s);
            const int drop = !first && ((al >= 6 && strncasecmp(s, "domain", 6) == 0) ||
                                        (al == 6 && strncasecmp(s, "secure", 6) == 0) ||
                                        (al >= 8 && strncasecmp(s, "samesite", 8) == 0) ||
                                        (al == 11 && strncasecmp(s, "partitioned", 11) == 0));
            if (!drop) {
               if (!first)
                  buf_str(&out, "; ");
               buf_add(&out, s, al);
            }
            first = 0;
            a = ae < e ? ae + 1 : e;
         }
         buf_str(&out, "\r\n");
         continue;
      }
      buf_add(&out, l, ll);
      buf_str(&out, "\r\n");
   }
   struct buf nb = {0};
   if (text && body.n) {
      if (html) {
         /* the shim first thing in <head> (or at the start) */
         const char *h = NULL;
         for (size_t i = 0; i + 5 < body.n && i < 65536; i++)
            if (body.p[i] == '<' && strncasecmp(body.p + i + 1, "head", 4) == 0 &&
                (body.p[i + 5] == '>' || body.p[i + 5] == ' ')) {
               h = memchr(body.p + i, '>', body.n - i);
               break;
            }
         const size_t at = h ? (size_t)(h - body.p) + 1 : 0;
         rewrite_links(body.p, at, &nb);
         buf_str(&nb, wr_shim);
         rewrite_links(body.p + at, body.n - at, &nb);
      } else {
         rewrite_links(body.p, body.n, &nb);
      }
      js_fix(&nb);
   } else {
      nb = body;
      body.p = NULL;
   }
   buf_fmt(&out, "Content-Length: %zu\r\nConnection: close\r\n", nb.n);
   if (html)
      buf_str(&out, "Referrer-Policy: unsafe-url\r\n");
   buf_str(&out, "\r\n");
   send_all(cfd, out.p, out.n);
   if (nb.n)
      send_all(cfd, nb.p, nb.n);
   if (status == 200 && cache_ok_request(method, host) && !ci_find(out.p, "\nSet-Cookie:") &&
       !ci_find(out.p, "no-store") && !ci_find(out.p, "private"))
      cache_store(host, path, &out, &nb);
   gettimeofday(&t1, NULL);
   const unsigned k = __atomic_add_fetch(&wr_requests, 1, __ATOMIC_RELAXED);
   if (k <= 300 || k % 50 == 0 || status >= 400)
      wr_log("web relay: #%u %s %s%.100s -> %d, %zu bytes, %ld ms", k, method, host, path, status, nb.n,
             (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
   free(nb.p);
done:
   free(req.p), free(hdr.p), free(body.p), free(out.p), free(upreq.p);
   shutdown(cfd, SHUT_WR);
   close(cfd);
}

static void *
conn_thread(void *arg)
{
   handle((int)(intptr_t)arg);
   return NULL;
}

static void *
accept_thread(void *arg)
{
   const int lfd = (int)(intptr_t)arg;
   pthread_attr_t at;
   pthread_attr_init(&at);
   pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
   pthread_attr_setstacksize(&at, 512 * 1024);
   for (;;) {
      const int fd = accept(lfd, NULL, NULL);
      if (fd < 0) {
         if (errno != EINTR)
            usleep(100000);
         continue;
      }
      pthread_t t;
      if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd) != 0)
         close(fd);
   }
   return NULL;
}

/* ---- a GET for the title itself (signed in, through the tunnel) ---------- */
int
web_relay_start(unsigned short port, const char *socks_host, unsigned short socks_port,
                const struct web_relay_hooks *hooks)
{
   if (hooks)
      wr_hooks = *hooks;
   if (socks_host)
      snprintf(wr_socks_host, sizeof(wr_socks_host), "%s", socks_host);
   wr_socks_port = socks_port;
   const int lfd = socket(AF_INET, SOCK_STREAM, 0);
   if (lfd < 0)
      return -1;
   int one = 1;
   setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in a;
   memset(&a, 0, sizeof(a));
#if defined(__FreeBSD__) || defined(__PROSPERO__)
   a.sin_len = sizeof(a);
#endif
   a.sin_family = AF_INET;
   a.sin_port = htons(port);
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 64) != 0) {
      wr_log("web relay: cannot listen on 127.0.0.1:%u (%d)", port, errno);
      close(lfd);
      return -1;
   }
   wr_port = port;
   snprintf(wr_origin, sizeof(wr_origin), "127.0.0.1:%u", port);
   if (hooks && hooks->cache_dir && hooks->cache_dir[0]) {
      /* the cache: emptied when it passes 300 MB (rewritten links carry the
       * port, so a different port starts it afresh too) */
      /* v3: app links rewritten, script fixes (older entries lack them) */
      snprintf(wr_cache_dir, sizeof(wr_cache_dir), "%s/%u-v3", hooks->cache_dir, port);
      mkdir(hooks->cache_dir, 0777);
      mkdir(wr_cache_dir, 0777);
      unsigned long long total = 0;
      unsigned files = 0;
      DIR *d = opendir(wr_cache_dir);
      if (d) {
         struct dirent *e;
         char fn[600];
         struct stat st;
         while ((e = readdir(d))) {
            snprintf(fn, sizeof(fn), "%s/%s", wr_cache_dir, e->d_name);
            if (e->d_name[0] != '.' && stat(fn, &st) == 0 && S_ISREG(st.st_mode))
               total += (unsigned long long)st.st_size, files++;
         }
         if (total > 300ull * 1024 * 1024) {
            rewinddir(d);
            while ((e = readdir(d)))
               if (e->d_name[0] != '.') {
                  snprintf(fn, sizeof(fn), "%s/%s", wr_cache_dir, e->d_name);
                  unlink(fn);
               }
            wr_log("web relay: cache emptied (%llu MB in %u files)", total >> 20, files);
            total = files = 0;
         }
         closedir(d);
      }
      wr_log("web relay: cache %s: %u files, %llu MB", wr_cache_dir, files, total >> 20);
   }
   pthread_t t;
   pthread_attr_t at;
   pthread_attr_init(&at);
   pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&t, &at, accept_thread, (void *)(intptr_t)lfd) != 0) {
      close(lfd);
      return -1;
   }
   wr_log("web relay: serving on http://%s/ through SOCKS %s:%u", wr_origin, wr_socks_host, socks_port);
   return 0;
}

#ifdef WEB_RELAY_HOST_TEST
/* WR_COOKIE_FILE: a "name=value; ..." file whose cookies are added (test account) */
static void
host_cookies(const char *host, char *out, size_t cap)
{
   (void)host;
   out[0] = 0;
   const char *fn = getenv("WR_COOKIE_FILE");
   FILE *f = fn ? fopen(fn, "r") : NULL;
   if (f) {
      if (!fgets(out, (int)cap, f))
         out[0] = 0;
      out[strcspn(out, "\r\n")] = 0;
      fclose(f);
   }
}
static void
host_link(const char *url)
{
   fprintf(stderr, "APP LINK: %s\n", url);
}
/* cc -DWEB_RELAY_HOST_TEST -I<bearssl>/inc web_relay.c <bearssl>/build/libbearssl.a -lpthread
 * ./a.out <port> <socks port>: no cookies (guest) */
int
main(int argc, char **argv)
{
   const unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 8900;
   const unsigned short sp = argc > 2 ? (unsigned short)atoi(argv[2]) : 1081;
   const struct web_relay_hooks h = {NULL, host_cookies, NULL, argc > 3 ? argv[3] : NULL, host_link};
   if (web_relay_start(port, "127.0.0.1", sp, &h) != 0)
      return 1;
   for (;;)
      pause();
}
#endif
