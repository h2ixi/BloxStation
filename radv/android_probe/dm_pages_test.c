#include "bionic_dm_pages.h"
#include <assert.h>
#include <stdio.h>

#define K 1024u

int main(void)
{
   const uintptr_t base = 0x10000000;
   struct bionic_dm_pages p = {0};

   /* one unmap of the whole mapping, no bitmap needed */
   assert(bionic_dm_pages_release(&p, base, 100 * K, base, base + 100 * K) == 1);
   assert(!p.bits);

   /* aligned-allocation pattern: trim head, trim tail, later free the middle */
   const size_t len = 3 * 1024 * K; /* 3 MiB mapped for a 1 MiB aligned block */
   const uintptr_t mid = base + 1024 * K, mid_end = mid + 1024 * K;
   assert(bionic_dm_pages_release(&p, base, len, base, mid) == 0);
   assert(bionic_dm_pages_release(&p, base, len, mid_end, base + len) == 0);
   assert(p.released == 512);
   assert(bionic_dm_pages_release(&p, base, len, mid, mid_end) == 1);
   bionic_dm_pages_clear(&p);
   assert(!p.bits && !p.released);

   /* repeated requests for the same range count once */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base, base + 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base, base + 16 * K) == 0);
   assert(p.released == 4);
   /* a range that only partly covers a page releases nothing of it */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 16 * K + 1, base + 20 * K - 1) == 0);
   assert(p.released == 4);
   /* re-mapping makes released pages live again; touched pages count */
   bionic_dm_pages_reuse(&p, base, 64 * K, base + 4 * K + 1, base + 4 * K + 2);
   assert(p.released == 3);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 16 * K, base + 64 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 4 * K, base + 8 * K) == 1);
   bionic_dm_pages_clear(&p);

   /* an unaligned length: an unmap reaching the end covers the last piece */
   assert(bionic_dm_pages_release(&p, base, 10 * K, base, base + 8 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 10 * K, base + 8 * K, base + 12 * K) == 1);
   bionic_dm_pages_clear(&p);

   /* ranges outside the mapping are ignored */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base - 64 * K, base) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 64 * K, base + 128 * K) == 0);
   assert(!p.bits);
   /* a range covering the mapping and beyond releases all of it */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base - 4 * K, base + 128 * K) == 1);

   bionic_dm_pages_clear(&p);

   /* taking released runs: nothing without a bitmap */
   assert(bionic_dm_pages_take(&p, base, 16 * K, 16 * K) == 0);
   /* 64 KiB mapping, the 32 KiB alignment trim pattern: head and tail freed */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base, base + 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 48 * K, base + 64 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 32 * K, 16 * K) == 0); /* no 32 KiB run */
   assert(bionic_dm_pages_take(&p, base, 16 * K, 16 * K) == base);
   assert(bionic_dm_pages_take(&p, base, 16 * K, 16 * K) == base + 48 * K);
   assert(p.released == 0);
   assert(bionic_dm_pages_take(&p, base, 4 * K, 4 * K) == 0);
   /* taken pages are live: releasing the rest does not free the region */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 16 * K, base + 48 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base, base + 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 48 * K, base + 64 * K) == 1);
   bionic_dm_pages_clear(&p);

   /* alignment: a run that is free but starts off the alignment is skipped */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 4 * K, base + 36 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 32 * K, 16 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 16 * K, 16 * K) == base + 16 * K);
   assert(bionic_dm_pages_take(&p, base, 8 * K, 4 * K) == base + 4 * K);
   assert(p.released == 2);
   /* left: pages at 12 and 32 KiB, not adjacent; 5 KiB needs two pages */
   assert(bionic_dm_pages_take(&p, base, 5 * K, 4 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 3 * K, 4 * K) == base + 12 * K);
   assert(bionic_dm_pages_take(&p, base, 4 * K, 4 * K) == base + 32 * K);
   assert(p.released == 0);
   bionic_dm_pages_clear(&p);

   /* a 4 KiB take from a free 16 KiB run marks one page live; the slack
    * stays released, so unmapping the 4 KiB frees the region */
   assert(bionic_dm_pages_release(&p, base, 32 * K, base + 4 * K, base + 32 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 4 * K, 16 * K) == base + 16 * K);
   assert(p.released == 6);
   /* the slack (20-32 KiB) is free but not on the alignment: no second take */
   assert(bionic_dm_pages_take(&p, base, 4 * K, 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 32 * K, base + 16 * K, base + 20 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 32 * K, base, base + 4 * K) == 1);
   bionic_dm_pages_clear(&p);

   /* free aligned blocks: pages 0-3 free, 4 live, 5-7 free -> one block */
   assert(bionic_dm_pages_free_blocks(&p, 4) == 0);
   assert(bionic_dm_pages_release(&p, base, 32 * K, base, base + 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 32 * K, base + 20 * K, base + 32 * K) == 0);
   assert(bionic_dm_pages_free_blocks(&p, 4) == 1);
   assert(bionic_dm_pages_free_blocks(&p, 1) == 7);
   bionic_dm_pages_clear(&p);

   /* a failed take is remembered until a release makes room */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base, base + 16 * K) == 0);
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 32 * K, base + 48 * K) == 0);
   assert(bionic_dm_pages_take(&p, base, 32 * K, 16 * K) == 0);
   assert(p.fail_need == 8 && p.fail_step == 4);
   assert(bionic_dm_pages_take(&p, base, 48 * K, 16 * K) == 0); /* larger: known to fail */
   assert(p.fail_need == 8);
   assert(bionic_dm_pages_take(&p, base, 32 * K, 4 * K) == 0); /* other step: scanned, replaces */
   assert(p.fail_step == 1 && p.fail_need == 8);
   /* a take that fits still succeeds and does not clear the failure */
   assert(bionic_dm_pages_take(&p, base, 16 * K, 16 * K) == base);
   assert(p.fail_need == 8);
   /* releasing the gap makes 32 KiB fit: the failure is forgotten */
   assert(bionic_dm_pages_release(&p, base, 64 * K, base + 16 * K, base + 32 * K) == 0);
   assert(p.fail_need == 0);
   assert(bionic_dm_pages_take(&p, base, 32 * K, 16 * K) == base + 16 * K);
   bionic_dm_pages_clear(&p);

   puts("PASS: whole, piecewise, repeated, partial-page, reuse, unaligned, out-of-range releases and taking runs");
   return 0;
}
