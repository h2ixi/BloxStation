/* Local web relay for the PS5 browser (web_relay.c).
 *
 * The PS5 browser cannot reach roblox.com (blocked on the console's own
 * network), so pages are served from http://127.0.0.1:<port>/<host>/<path>:
 * the relay fetches https://<host>/<path> over TLS through the in-title
 * SOCKS5 gateway (the WARP tunnel), adds the signed-in account's cookie, and
 * rewrites Roblox links in pages so that they come back through it too. */
#ifndef WEB_RELAY_H
#define WEB_RELAY_H
#include <stddef.h>

struct web_relay_hooks {
   void (*log)(const char *line);
   /* "name=value; name2=value2" for a request to host (NULL/empty: none) */
   void (*cookies)(const char *host, char *out, size_t cap);
   void (*random)(void *buf, size_t len);
   const char *cache_dir; /* Roblox's static files kept here (NULL: none) */
   /* a page navigated to roblox:// or robloxmobile:// (Join, ...): for the app */
   void (*app_link)(const char *url);
};

/* Listens on 127.0.0.1:port and serves in background threads: 0, or -1. */
int web_relay_start(unsigned short port, const char *socks_host, unsigned short socks_port,
                    const struct web_relay_hooks *hooks);
/* https://www.roblox.com/x -> http://127.0.0.1:port/www.roblox.com/x for the
 * relayed hosts; anything else is copied unchanged. Returns out. */
char *web_relay_url(const char *url, char *out, size_t cap);
/* a relayed host (roblox.com, rbxcdn.com and their subdomains)? */
int web_relay_host_ok(const char *host, size_t len);
#endif
