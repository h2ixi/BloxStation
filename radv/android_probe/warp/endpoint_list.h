/* WARP endpoint candidates for countries/ISPs that block some of them.
 *
 * Cloudflare serves the same WARP identity on several anycast ranges and on
 * many UDP ports; networks that block WireGuard usually block one port or one
 * range, not all of them. Candidates are interleaved so consecutive tries
 * change both the address range and the port.
 *
 * The endpoint that last brought the tunnel up is saved (plain "ip:port",
 * not secret) so the next start goes straight to what works here. */
#ifndef ROBLOX_WARP_ENDPOINT_LIST_H
#define ROBLOX_WARP_ENDPOINT_LIST_H
#include <stdio.h>
#include <string.h>
#include "../gateway_config.h"

static const char *const warp_endpoint_ranges[] = {
   "188.114.98.", "162.159.192.", "188.114.97.", "162.159.195.",
   "188.114.99.", "162.159.193.", "188.114.96.",
};
static const unsigned char warp_endpoint_hosts[] = {1, 2, 3, 5, 7, 9, 10, 20};
static const unsigned short warp_endpoint_ports[] = {
   2408, 500,  1701, 4500, 854,  859,  864,  878,  880,  890,  891,  894,  903,  908,
   928,  934,  939,  942,  943,  945,  946,  955,  968,  987,  988,  1002, 1010, 1014,
   1018, 1070, 1074, 1180, 1387, 1843, 2371, 2506, 3138, 3476, 3581, 3854, 4177, 4198,
   4233, 5279, 5956, 7103, 7152, 7156, 7281, 7559, 8319, 8742, 8854, 8886,
};
#define WARP_EP_NR (sizeof(warp_endpoint_ranges) / sizeof(warp_endpoint_ranges[0]))
#define WARP_EP_NH (sizeof(warp_endpoint_hosts) / sizeof(warp_endpoint_hosts[0]))
#define WARP_EP_NP (sizeof(warp_endpoint_ports) / sizeof(warp_endpoint_ports[0]))

static unsigned
warp_endpoint_candidate_count(void)
{
   return (unsigned)(WARP_EP_NR * WARP_EP_NP);
}

/* Candidate i = q * ranges + r: range r, port (q + 8r) % ports, so every
 * range/port pair comes up exactly once and consecutive tries change both;
 * the host byte varies with q. */
static int
warp_endpoint_candidate(unsigned i, char *ip, size_t cap, unsigned short *port)
{
   if (i >= warp_endpoint_candidate_count() || !ip || !port)
      return -1;
   const unsigned r = i % WARP_EP_NR, q = i / WARP_EP_NR;
   const int w = snprintf(ip, cap, "%s%u", warp_endpoint_ranges[r], warp_endpoint_hosts[q % WARP_EP_NH]);
   if (w < 0 || (size_t)w >= cap)
      return -1;
   *port = warp_endpoint_ports[(q + 8 * r) % WARP_EP_NP];
   return 0;
}

static int
warp_endpoint_load(const char *path, char *ip, size_t cap, unsigned short *port)
{
   char line[64] = {0};
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   const int ok = fgets(line, sizeof(line), f) != NULL;
   fclose(f);
   if (!ok)
      return -1;
   line[strcspn(line, "\r\n")] = 0;
   struct in_addr a;
   if (!strchr(line, ':') || gateway_config_parse(line, &a, port) != 0)
      return -1;
   return inet_ntop(AF_INET, &a, ip, (socklen_t)cap) ? 0 : -1;
}

/* Written to a temporary name and renamed, so a crash never leaves half a line. */
static int
warp_endpoint_save(const char *path, const char *ip, unsigned short port)
{
   char tmp[512];
   if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
      return -1;
   FILE *f = fopen(tmp, "w");
   if (!f)
      return -1;
   const int ok = fprintf(f, "%s:%u\n", ip, port) > 0;
   if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
      remove(tmp);
      return -1;
   }
   return 0;
}
#endif
