/* The on-screen guide: pages drawn by tools/make-guide-pages.py
 * (guide_pages.h) and copied over Roblox's frames by roblox_vulkan.h, so no
 * web page is involved (the PS5 in-game browser sometimes crashes with
 * CE-108262-9). It opens from Guide in the left menu (tools/patch-ui.sh)
 * and, once per player, the first time a player reaches Home. While it is open the controller belongs to it:
 * L1 / R1 or the D-pad left / right turn pages, Circle closes it. */
#ifndef ROBLOX_GUIDE_H
#define ROBLOX_GUIDE_H
#include "guide_pages.h"

static int rg_page = -1; /* the page shown, -1 closed */

static void
rg_open(int page)
{
   if (page < 0 || page >= GUIDE_PAGES)
      page = 0;
   __atomic_store_n(&rg_page, page, __ATOMIC_RELEASE);
   report("roblox: guide: open (page %d)", page + 1);
}

static int
rg_is_open(void)
{
   return __atomic_load_n(&rg_page, __ATOMIC_ACQUIRE) >= 0;
}

/* one page into 32-bit pixels (bgra: B8G8R8A8, else R8G8B8A8) */
static void
rg_decode(int page, uint32_t *dst, int bgra)
{
   const uint8_t *pal = guide_pal[page], *p = guide_rle[page];
   uint32_t col[256];
   for (int i = 0; i < 256; i++) {
      const uint32_t r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
      col[i] = 0xFF000000u | (bgra ? r << 16 | g << 8 | b : b << 16 | g << 8 | r);
   }
   const size_t total = (size_t)GUIDE_W * GUIDE_H;
   size_t o = 0;
   for (uint32_t i = 0; i + 1 < guide_rle_len[page] && o < total; i += 2) { /* (length - 1, colour) */
      size_t n = (size_t)p[i] + 1;
      const uint32_t c = col[p[i + 1]];
      if (n > total - o)
         n = total - o;
      for (size_t k = 0; k < n; k++)
         dst[o++] = c;
   }
   while (o < total)
      dst[o++] = col[0];
}

/* From the pad thread with the buttons held now: 1 when the guide takes the
 * input (Roblox must not see it), 0 when Roblox gets it. After Circle closes
 * the guide the buttons stay with it until all are released, so that Circle
 * does not also go Back in the menus. */
static int
rg_pad(uint32_t buttons)
{
   static uint32_t prev;
   static int swallow;
   const uint32_t down = buttons & ~prev;
   prev = buttons;
   int page = __atomic_load_n(&rg_page, __ATOMIC_ACQUIRE);
   if (page < 0) {
      if (swallow && !buttons)
         swallow = 0;
      return swallow;
   }
   swallow = 1;
   if (down & 0x2000u) { /* Circle */
      __atomic_store_n(&rg_page, -1, __ATOMIC_RELEASE);
      report("roblox: guide: closed");
      return 1;
   }
   if ((down & (0x0800u | 0x0020u)) && page + 1 < GUIDE_PAGES) /* R1, right */
      page++;
   if ((down & (0x0400u | 0x0080u)) && page > 0) /* L1, left */
      page--;
   __atomic_store_n(&rg_page, page, __ATOMIC_RELEASE);
   return 1;
}
#endif
