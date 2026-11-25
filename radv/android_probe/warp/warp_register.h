#ifndef ROBLOX_WARP_REGISTER_H
#define ROBLOX_WARP_REGISTER_H
/* On-console first-run Cloudflare WARP registration.
 *
 * warp_first_run_register(dir) does nothing (and touches no network) when
 * <dir>/warp.json exists. Otherwise it:
 *   1. reuses <dir>/warp-pending.json, or creates it once (X25519 key, 0600,
 *      O_EXCL). An existing pending key is never regenerated;
 *   2. POSTs the public key to api.cloudflareclient.com/v0a2158/reg over
 *      BearSSL TLS (certificate and host name verified);
 *   3. strictly validates the JSON reply and stores the result in
 *      <dir>/warp-staged.json (0600, O_EXCL) so a completed registration
 *      survives a failed step 4;
 *   4. PATCHes warp_enabled=true;
 *   5. creates <dir>/warp.json (0600, O_EXCL, never overwritten);
 *   6. only then removes the staged and pending files.
 * dir must be a trusted private directory. Nothing is logged; key and token
 * buffers are wiped before return.
 *
 * Returns: 1 warp.json already present (no network), 0 registered now, or
 *  -1 bad argument / path / stat failure
 *  -2 entropy or pending-key creation failed
 *  -3 pending file unreadable or invalid (left untouched for recovery)
 *  -4 DNS / TCP connection failed (registration POST)
 *  -5 TLS setup failed (allocation, clock, engine reset)
 *  -6 TLS handshake or request write failed (incl. untrusted certificate)
 *  -7 HTTP response unreadable, malformed or too large
 *  -8 registration POST returned a non-2xx status
 *  -9 registration reply invalid (JSON, endpoint, key, address, id, token)
 * -10 PATCH warp_enabled failed (any transport error or non-2xx status)
 * -11 creating warp-staged.json or warp.json failed
 * -12 warp.json is durable but leftover pending/staged removal failed */
int warp_first_run_register(const char *dir);

#ifdef WARP_REGISTER_TEST
/* Host tests only: never compiled into the title. */
#include <bearssl.h>
struct warp_register_test_hooks {
   const char *connect_ip;               /* replaces DNS and fixed fallbacks */
   unsigned short port;                  /* replaces 443 */
   const br_x509_trust_anchor *tas;      /* replaces the Mozilla anchors */
   size_t tas_num;
   const char *entropy_path;             /* replaces /dev/urandom */
};
extern struct warp_register_test_hooks warp_register_test_hooks;
#endif
#endif
