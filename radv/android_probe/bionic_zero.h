/* Zero-filling new direct-memory mappings. Linux hands out zeroed anonymous
 * memory lazily, page by page as it is touched; direct memory comes back with
 * old contents, so every mapping is cleared up front, often far more than the
 * engine touches soon. Large clears use non-temporal stores: they go straight
 * to memory instead of evicting the caches the game is using, and do not
 * read each line first. Small clears stay in the cache, where the engine is
 * about to write them. Independent of the console runtime for host testing. */
#ifndef BIONIC_ZERO_H
#define BIONIC_ZERO_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>

#define BIONIC_ZERO_STREAM_MIN (256u << 10)

static void
bionic_zero_fill(void *p, size_t n)
{
   uint8_t *d = p;
   if (n < BIONIC_ZERO_STREAM_MIN) {
      memset(d, 0, n);
      return;
   }
   /* head up to 64-byte alignment, 64-byte streamed body, tail */
   const size_t head = (size_t)(-(uintptr_t)d & 63u);
   memset(d, 0, head);
   d += head;
   n -= head;
   const __m128i z = _mm_setzero_si128();
   uint8_t *const end = d + (n & ~(size_t)63);
   for (; d < end; d += 64) {
      _mm_stream_si128((__m128i *)(void *)d, z);
      _mm_stream_si128((__m128i *)(void *)(d + 16), z);
      _mm_stream_si128((__m128i *)(void *)(d + 32), z);
      _mm_stream_si128((__m128i *)(void *)(d + 48), z);
   }
   _mm_sfence(); /* streamed stores are weakly ordered: publish before return */
   memset(d, 0, n & 63u);
}

#endif
