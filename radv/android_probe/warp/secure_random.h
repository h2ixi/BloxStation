/* Fail-closed OS entropy reader for future per-console key generation.
 * Never substitute timing jitter or a non-cryptographic PRNG for this. */
#ifndef ROBLOX_WARP_SECURE_RANDOM_H
#define ROBLOX_WARP_SECURE_RANDOM_H
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

static int
warp_secure_random_from_path(const char *path, void *output, size_t size)
{
   unsigned char *buf = output;
   size_t done = 0;
   int fd;
   if (!path || (!output && size))
      return -1;
   fd = open(path, O_RDONLY);
   if (fd >= 0) {
      while (done < size) {
         const ssize_t n = read(fd, buf + done, size - done);
         if (n < 0 && errno == EINTR)
            continue;
         if (n <= 0)
            break;
         done += (size_t)n;
      }
      close(fd);
      if (done == size)
         return 0;
   }
   /* Clear all output, including any bytes read before the failure. */
   for (size_t i = 0; i < size; i++)
      ((volatile unsigned char *)buf)[i] = 0;
   return -1;
}
#endif
