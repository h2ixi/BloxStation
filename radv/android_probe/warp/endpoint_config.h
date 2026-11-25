/* Select a validated WARP endpoint; prefer the device registration. */
#ifndef ROBLOX_WARP_ENDPOINT_CONFIG_H
#define ROBLOX_WARP_ENDPOINT_CONFIG_H
#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../gateway_config.h"

static int
warp_endpoint_port(const char *text, unsigned short *port)
{
   unsigned n = 0;
   if (!text || !*text || !port)
      return -1;
   for (; *text; text++) {
      if (*text < '0' || *text > '9')
         return -1;
      n = n * 10 + (unsigned)(*text - '0');
      if (n > 65535)
         return -1;
   }
   if (!n)
      return -1;
   *port = (unsigned short)n;
   return 0;
}

static int
warp_endpoint_select(const char *registered_ip, unsigned registered_port, const char *override,
                     char *out_ip, size_t out_cap, unsigned short *out_port)
{
   struct in_addr parsed;
   unsigned short port;
   if (!registered_ip || !out_ip || !out_cap || !out_port ||
       registered_port == 0 || registered_port > 65535 ||
       inet_pton(AF_INET, registered_ip, &parsed) != 1)
      return -1;
   if (override && *override) {
      if (!strchr(override, ':') || gateway_config_parse(override, &parsed, &port) != 0)
         return -1;
   } else {
      port = (unsigned short)registered_port;
   }
   if (!inet_ntop(AF_INET, &parsed, out_ip, out_cap))
      return -1;
   *out_port = port;
   return 0;
}
#endif
