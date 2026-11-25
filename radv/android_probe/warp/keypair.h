/* Per-device keypair primitive for future local provisioning.
 * The caller must validate that entropy_path names a cryptographic OS random
 * device, keep the private key secret, and securely erase it after use. */
#ifndef ROBLOX_WARP_KEYPAIR_H
#define ROBLOX_WARP_KEYPAIR_H
#include <stddef.h>
#include "secure_random.h"
#include "crypto/refc/x25519.h"

static int
warp_keypair_from_entropy(const char *entropy_path, unsigned char private_key[32], unsigned char public_key[32])
{
   if (!private_key || !public_key || private_key == public_key)
      return -1;
   for (size_t i = 0; i < 32; i++)
      private_key[i] = public_key[i] = 0;
   if (warp_secure_random_from_path(entropy_path, private_key, 32) == 0 &&
       x25519_base(public_key, private_key, 1) == 0)
      return 0;
   for (size_t i = 0; i < 32; i++) {
      ((volatile unsigned char *)private_key)[i] = 0;
      ((volatile unsigned char *)public_key)[i] = 0;
   }
   return -1;
}
#endif
