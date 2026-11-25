/* Address-sorted index of direct-memory regions, so finding the regions a
 * range overlaps is a binary search instead of a walk over every region
 * (thousands are live in a game, and unmaps run thousands of times a second).
 * Regions never overlap, so sorting by base also sorts by end. Insertions and
 * removals move entries; they are rare next to lookups, since most mappings
 * are served from released pages or the region cache. Independent of the
 * console runtime for host testing. */
#ifndef BIONIC_DM_INDEX_H
#define BIONIC_DM_INDEX_H
#include <stdint.h>
#include <string.h>

struct bionic_dm_ent {
   uintptr_t base, end;
   uint32_t slot;
};

struct bionic_dm_index {
   struct bionic_dm_ent *e;
   uint32_t n, max;
};

/* First position whose region ends after a: the first one a range starting
 * at a can overlap. */
static uint32_t
bionic_dm_index_first(const struct bionic_dm_index *x, uintptr_t a)
{
   uint32_t lo = 0, hi = x->n;
   while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (x->e[mid].end <= a)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo;
}

/* Returns 0, or -1 if full. */
static int
bionic_dm_index_add(struct bionic_dm_index *x, uintptr_t base, uintptr_t end, uint32_t slot)
{
   if (x->n == x->max)
      return -1;
   const uint32_t k = bionic_dm_index_first(x, base);
   memmove(&x->e[k + 1], &x->e[k], (size_t)(x->n - k) * sizeof(*x->e));
   x->e[k].base = base;
   x->e[k].end = end;
   x->e[k].slot = slot;
   x->n++;
   return 0;
}

static void
bionic_dm_index_del_at(struct bionic_dm_index *x, uint32_t k)
{
   memmove(&x->e[k], &x->e[k + 1], (size_t)(x->n - k - 1) * sizeof(*x->e));
   x->n--;
}

/* Remove the region starting at base; returns 0, or -1 if absent. */
static inline int
bionic_dm_index_del(struct bionic_dm_index *x, uintptr_t base)
{
   const uint32_t k = bionic_dm_index_first(x, base);
   if (k == x->n || x->e[k].base != base)
      return -1;
   bionic_dm_index_del_at(x, k);
   return 0;
}

/* Slot of the region containing a, or -1. */
static int64_t
bionic_dm_index_find(const struct bionic_dm_index *x, uintptr_t a)
{
   const uint32_t k = bionic_dm_index_first(x, a);
   if (k < x->n && x->e[k].base <= a)
      return x->e[k].slot;
   return -1;
}

#endif
