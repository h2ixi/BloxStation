/* Validate explicitly configured SOCKS gateway addresses. */
#ifndef ROBLOX_GATEWAY_CONFIG_H
#define ROBLOX_GATEWAY_CONFIG_H
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

static int
gateway_config_parse(const char *value, struct in_addr *ip, unsigned short *port)
{
   char host[64];
   const char *sep;
   size_t n;
   unsigned long p = 1080;
   if (!value || !*value || !ip || !port)
      return -1;
   sep = strchr(value, ':');
   n = sep ? (size_t)(sep - value) : strlen(value);
   if (!n || n >= sizeof(host))
      return -1;
   memcpy(host, value, n);
   host[n] = 0;
   if (sep) {
      const char *s = sep + 1;
      if (!*s)
         return -1;
      p = 0;
      for (; *s; s++) {
         if (*s < '0' || *s > '9')
            return -1;
         p = p * 10 + (unsigned)(*s - '0');
         if (p > 65535)
            return -1;
      }
      if (!p)
         return -1;
   }
   if (inet_pton(AF_INET, host, ip) != 1)
      return -1;
   *port = (unsigned short)p;
   return 0;
}
#endif
