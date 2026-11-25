/* First-run private key preparation. No network or account operations. */
#ifndef ROBLOX_WARP_PROVISION_PREPARE_H
#define ROBLOX_WARP_PROVISION_PREPARE_H
#include <stdio.h>
#include "keypair.h"
#include "identity_store.h"

/* The caller must check that no completed identity exists and supply an OS
 * random device plus a path in a trusted private directory. A pending identity
 * is never replaced; it must be recovered rather than silently regenerated. */
static void warp_provision_b64_32(const unsigned char in[32], char out[45])
{
   static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   size_t o = 0;
   for (size_t i = 0; i < 32; i += 3) {
      unsigned value = (unsigned)in[i] << 16;
      if (i + 1 < 32) value |= (unsigned)in[i + 1] << 8;
      if (i + 2 < 32) value |= in[i + 2];
      out[o++] = alphabet[(value >> 18) & 63];
      out[o++] = alphabet[(value >> 12) & 63];
      out[o++] = i + 1 < 32 ? alphabet[(value >> 6) & 63] : '=';
      out[o++] = i + 2 < 32 ? alphabet[value & 63] : '=';
   }
   out[o] = 0;
}

static int warp_provision_prepare(const char *entropy_path, const char *pending_path)
{
   unsigned char private_key[32] = {0}, public_key[32] = {0};
   char priv64[45] = {0}, pub64[45] = {0}, json[160] = {0};
   int result = -1;
   if (warp_keypair_from_entropy(entropy_path, private_key, public_key) != 0)
      goto out;
   warp_provision_b64_32(private_key, priv64);
   warp_provision_b64_32(public_key, pub64);
   int size = snprintf(json, sizeof(json), "{\"private_key\":\"%s\",\"public_key\":\"%s\"}\n", priv64, pub64);
   if (size > 0 && (size_t)size < sizeof(json))
      result = warp_identity_create(pending_path, json, (size_t)size);
out:
   for (size_t i = 0; i < sizeof(private_key); i++)
      ((volatile unsigned char *)private_key)[i] = 0;
   for (size_t i = 0; i < sizeof(public_key); i++)
      ((volatile unsigned char *)public_key)[i] = 0;
   for (size_t i = 0; i < sizeof(priv64); i++)
      ((volatile char *)priv64)[i] = 0;
   for (size_t i = 0; i < sizeof(pub64); i++)
      ((volatile char *)pub64)[i] = 0;
   for (size_t i = 0; i < sizeof(json); i++)
      ((volatile char *)json)[i] = 0;
   return result;
}
#endif
