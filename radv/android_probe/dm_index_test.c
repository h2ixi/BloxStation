/* Host test for bionic_dm_index.h: random adds, removals and lookups checked
 * against a plain array walked in full.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *    -o /tmp/dm-index-test radv/android_probe/dm_index_test.c && /tmp/dm-index-test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "bionic_dm_index.h"

#define SLOTS 512
#define UNIT 0x10000u

static uintptr_t ref_base[SLOTS], ref_end[SLOTS]; /* base 0: slot unused */

static int
overlaps_free(uintptr_t b, uintptr_t e)
{
   for (int i = 0; i < SLOTS; i++)
      if (ref_base[i] && b < ref_end[i] && e > ref_base[i])
         return 0;
   return 1;
}

int main(void)
{
   static struct bionic_dm_ent ents[SLOTS];
   struct bionic_dm_index x = {ents, 0, SLOTS};
   srand(7);

   /* empty index */
   assert(bionic_dm_index_find(&x, 0x1000) == -1);
   assert(bionic_dm_index_first(&x, 0x1000) == 0);
   assert(bionic_dm_index_del(&x, 0x1000) == -1);

   for (int step = 0; step < 200000; step++) {
      const int op = rand() % 3;
      const int s = rand() % SLOTS;
      if (op == 0 && !ref_base[s]) {
         const uintptr_t b = (uintptr_t)(1 + rand() % 4000) * UNIT;
         const uintptr_t e = b + (uintptr_t)(1 + rand() % 8) * UNIT;
         if (!overlaps_free(b, e))
            continue;
         assert(bionic_dm_index_add(&x, b, e, (uint32_t)s) == 0);
         ref_base[s] = b;
         ref_end[s] = e;
      } else if (op == 1 && ref_base[s]) {
         assert(bionic_dm_index_del(&x, ref_base[s]) == 0);
         ref_base[s] = 0;
      } else {
         /* lookups: containment, and the overlap walk used by unmap */
         const uintptr_t a = (uintptr_t)(rand() % (4010 * 16)) * (UNIT / 16);
         const uintptr_t e = a + (uintptr_t)(1 + rand() % 64) * (UNIT / 16);
         int64_t want = -1;
         int overlap = 0;
         for (int i = 0; i < SLOTS; i++) {
            if (ref_base[i] && a >= ref_base[i] && a < ref_end[i])
               want = i;
            if (ref_base[i] && a < ref_end[i] && e > ref_base[i])
               overlap++;
         }
         assert(bionic_dm_index_find(&x, a) == want);
         int seen = 0;
         for (uint32_t k = bionic_dm_index_first(&x, a); k < x.n && x.e[k].base < e; k++) {
            const uint32_t sl = x.e[k].slot;
            assert(ref_base[sl] == x.e[k].base && a < ref_end[sl] && e > ref_base[sl]);
            seen++;
         }
         assert(seen == overlap);
      }
      for (uint32_t k = 1; k < x.n; k++)
         assert(x.e[k - 1].end <= x.e[k].base);
   }

   /* full index refuses */
   struct bionic_dm_ent one[1];
   struct bionic_dm_index y = {one, 0, 1};
   assert(bionic_dm_index_add(&y, UNIT, 2 * UNIT, 0) == 0);
   assert(bionic_dm_index_add(&y, 3 * UNIT, 4 * UNIT, 1) == -1);

   puts("PASS: index add, remove, find and overlap walks match a full scan");
   return 0;
}
