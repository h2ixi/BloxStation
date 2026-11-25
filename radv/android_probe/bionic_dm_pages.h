/* Which 4 KiB pages of a direct-memory region the engine has unmapped.
 * Allocators map extra length for alignment, then unmap the head, the tail
 * and later the aligned middle as separate calls; none of those is a single
 * unmap of the whole original mapping. The region can only be returned to the
 * system once every page has been released. Independent of the console
 * runtime for host testing. */
#ifndef BIONIC_DM_PAGES_H
#define BIONIC_DM_PAGES_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BIONIC_DM_PAGE 4096u

struct bionic_dm_pages {
   uint8_t *bits; /* NULL until the first partial release */
   uint32_t released, total;
   /* The smallest run (in pages, for runs aligned to fail_step pages) a take
    * found no room for. Only a release can make room, so until the next one
    * takes as large or larger fail without scanning. 0: nothing known. */
   uint32_t fail_need, fail_step;
};

/* Mark [start, end) of the mapping at base (original length len) released.
 * Pages are counted only when fully inside the range, except the final
 * partial page of the mapping, which goes with an unmap reaching its end.
 * Returns 1 when the whole mapping is released, 0 if pages remain, -1 if
 * the bitmap could not be allocated (nothing is then considered released). */
static int
bionic_dm_pages_release(struct bionic_dm_pages *p, uintptr_t base, size_t len, uintptr_t start, uintptr_t end)
{
   const uintptr_t map_end = base + len;
   if (start < base)
      start = base;
   if (end > map_end)
      end = map_end;
   if (end <= start)
      return 0;
   const uint32_t total = (uint32_t)((len + BIONIC_DM_PAGE - 1) / BIONIC_DM_PAGE);
   const uint32_t first = (uint32_t)((start - base + BIONIC_DM_PAGE - 1) / BIONIC_DM_PAGE);
   const uint32_t last = end == map_end ? total : (uint32_t)((end - base) / BIONIC_DM_PAGE);
   if (first == 0 && last == total && !p->bits)
      return 1;
   if (last <= first)
      return 0;
   if (!p->bits) {
      p->bits = calloc((total + 7) / 8, 1);
      if (!p->bits)
         return -1;
      p->total = total;
      p->released = 0;
   }
   for (uint32_t i = first; i < last; i++) {
      const uint8_t m = (uint8_t)(1u << (i & 7));
      if (!(p->bits[i >> 3] & m)) {
         p->bits[i >> 3] |= m;
         p->released++;
         p->fail_need = 0;
      }
   }
   return p->released == p->total;
}

/* A MAP_FIXED mapping over part of the region makes those pages live again.
 * Every page the range touches counts, so a live page is never freed. */
static void
bionic_dm_pages_reuse(struct bionic_dm_pages *p, uintptr_t base, size_t len, uintptr_t start, uintptr_t end)
{
   const uintptr_t map_end = base + len;
   if (!p->bits)
      return;
   if (start < base)
      start = base;
   if (end > map_end)
      end = map_end;
   if (end <= start)
      return;
   const uint32_t first = (uint32_t)((start - base) / BIONIC_DM_PAGE);
   const uint32_t last = (uint32_t)((end - base + BIONIC_DM_PAGE - 1) / BIONIC_DM_PAGE);
   for (uint32_t i = first; i < last && i < p->total; i++) {
      const uint8_t m = (uint8_t)(1u << (i & 7));
      if (p->bits[i >> 3] & m) {
         p->bits[i >> 3] &= (uint8_t)~m;
         p->released--;
      }
   }
}

/* Hand out a run of released pages for a new mapping of len bytes. The run
 * starts on a multiple of align bytes (a multiple of the page) from base and
 * len rounded up to align must be free, but only the pages len covers are
 * marked live: the engine later unmaps len, and slack pages left live would
 * keep the region from ever being freed. Returns the address, or 0. */
static uintptr_t
bionic_dm_pages_take(struct bionic_dm_pages *p, uintptr_t base, size_t len, size_t align)
{
   if (!p->bits || !len)
      return 0;
   const uint32_t step = (uint32_t)(align / BIONIC_DM_PAGE);
   if (!step)
      return 0;
   const uint32_t live = (uint32_t)((len + BIONIC_DM_PAGE - 1) / BIONIC_DM_PAGE);
   const uint32_t need = (live + step - 1) / step * step;
   if (need > p->released || (p->fail_need && step == p->fail_step && need >= p->fail_need))
      return 0;
   for (uint32_t s = 0; s + need <= p->total; s += step) {
      uint32_t i = s;
      while (i < s + need && (p->bits[i >> 3] & (1u << (i & 7))))
         i++;
      if (i < s + need) {
         s = (i / step) * step; /* the next start past the live page */
         continue;
      }
      for (i = s; i < s + live; i++)
         p->bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
      p->released -= live;
      return base + (uintptr_t)s * BIONIC_DM_PAGE;
   }
   if (step != p->fail_step || !p->fail_need || need < p->fail_need) {
      p->fail_need = need;
      p->fail_step = step;
   }
   return 0;
}

/* How many whole aligned blocks of step pages are fully released: the part
 * of the released pages a new mapping could still be placed in. */
static uint32_t
bionic_dm_pages_free_blocks(const struct bionic_dm_pages *p, uint32_t step)
{
   uint32_t n = 0;
   if (!p->bits || !step)
      return 0;
   for (uint32_t s = 0; s + step <= p->total; s += step) {
      uint32_t i = s;
      while (i < s + step && (p->bits[i >> 3] & (1u << (i & 7))))
         i++;
      n += i == s + step;
   }
   return n;
}

static void
bionic_dm_pages_clear(struct bionic_dm_pages *p)
{
   free(p->bits);
   memset(p, 0, sizeof(*p));
}
#endif
