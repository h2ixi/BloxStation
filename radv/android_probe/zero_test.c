/* Host test and timing for bionic_zero.h.
 * cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *    -o /tmp/zero-test radv/android_probe/zero_test.c && /tmp/zero-test */
#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bionic_zero.h"

static double
now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
   enum { CAP = 4 << 20 };
   uint8_t *buf = malloc(CAP + 256);
   assert(buf);
   /* every offset/length mix around the alignment and the streaming cutoff:
    * exactly [off, off+n) is cleared, the guard bytes around it are not */
   const size_t lens[] = {0, 1, 63, 64, 65, 4096, BIONIC_ZERO_STREAM_MIN - 1, BIONIC_ZERO_STREAM_MIN,
                          BIONIC_ZERO_STREAM_MIN + 1, BIONIC_ZERO_STREAM_MIN + 63, (1u << 20) + 17, CAP};
   for (size_t li = 0; li < sizeof(lens) / sizeof(*lens); li++)
      for (size_t off = 64; off < 64 + 70; off += 7) {
         const size_t n = lens[li];
         memset(buf, 0xA5, CAP + 256);
         bionic_zero_fill(buf + off, n);
         for (size_t i = 0; i < off; i++)
            assert(buf[i] == 0xA5);
         for (size_t i = off; i < off + n; i++)
            assert(buf[i] == 0);
         for (size_t i = off + n; i < off + n + 64 && i < CAP + 256; i++)
            assert(buf[i] == 0xA5);
      }
   free(buf);

   /* rough speed against memset, 64 MiB in 2 MiB pieces */
   enum { BIG = 64 << 20, PIECE = 2 << 20 };
   uint8_t *big = malloc(BIG);
   assert(big);
   memset(big, 1, BIG);
   double t = now();
   for (int r = 0; r < 4; r++)
      for (size_t o = 0; o < BIG; o += PIECE)
         memset(big + o, 0, PIECE);
   const double tm = now() - t;
   t = now();
   for (int r = 0; r < 4; r++)
      for (size_t o = 0; o < BIG; o += PIECE)
         bionic_zero_fill(big + o, PIECE);
   const double ts = now() - t;
   printf("memset %.1f GB/s, streamed %.1f GB/s (host, 2 MiB pieces)\n", 4.0 * BIG / tm / 1e9,
          4.0 * BIG / ts / 1e9);
   free(big);
   puts("PASS: zero fill clears exactly the range for all alignments and sizes");
   return 0;
}
