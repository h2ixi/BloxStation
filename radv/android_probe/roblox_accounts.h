/* Keeping the account switcher's list across restarts.
 *
 * The app keeps its signed-in accounts in an opaque, server-issued token,
 * appStorage.json "AccountBlob". After a restart the list could come back
 * with only the last account (the blob went from two accounts' size back
 * to one). Here the largest blob seen while the app runs is kept in
 * /data/roblox/files/accountblob.bak, and at start, when the stored blob is
 * smaller than that copy, the copy is put back before the app reads it.
 * A sign-out (DID_LOG_OUT) drops the copy, so signing out still works.
 * ROBLOX_NO_ACCOUNT_KEEP=1 turns it off. The blob is a credential: only
 * lengths are logged. */
#ifndef ROBLOX_ACCOUNTS_H
#define ROBLOX_ACCOUNTS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RA_STORAGE "/data/roblox/files/appData/LocalStorage/appStorage.json"
#define RA_BACKUP "/data/roblox/files/accountblob.bak"

static char *
ra_read_file(const char *path, long *len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   const long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
   if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
      free(b);
      b = NULL;
   }
   fclose(f);
   if (b) {
      b[n] = 0;
      if (len)
         *len = n;
   }
   return b;
}

/* the AccountBlob value in appStorage.json (pointer into buf, length out) */
static char *
ra_find_blob(char *buf, size_t *vlen)
{
   static const char key[] = "\"AccountBlob\":\"";
   char *p = buf ? strstr(buf, key) : NULL;
   if (!p)
      return NULL;
   p += sizeof(key) - 1;
   char *e = strchr(p, '"');
   if (!e)
      return NULL;
   *vlen = (size_t)(e - p);
   return p;
}

/* at start, before the app reads appStorage.json */
static void
ra_restore_at_start(void)
{
   if (getenv("ROBLOX_NO_ACCOUNT_KEEP"))
      return;
   long n = 0, bn = 0;
   char *buf = ra_read_file(RA_STORAGE, &n);
   char *bak = ra_read_file(RA_BACKUP, &bn);
   size_t cur = 0;
   char *v = ra_find_blob(buf, &cur);
   if (bak)
      bak[strcspn(bak, "\r\n\"")] = 0;
   const size_t blen = bak ? strlen(bak) : 0;
   report("roblox: accounts: saved list %zu bytes, kept copy %zu bytes", cur, blen);
   if (v && blen > cur) {
      FILE *o = fopen(RA_STORAGE ".new", "wb");
      if (o) {
         fwrite(buf, 1, (size_t)(v - buf), o);
         fwrite(bak, 1, blen, o);
         fwrite(v + cur, 1, (size_t)(n - (v - buf) - (long)cur), o);
         fclose(o);
         rename(RA_STORAGE ".new", RA_STORAGE);
         report("roblox: accounts: account list put back from the kept copy");
      }
   }
   free(buf);
   free(bak);
}

/* every few seconds while the app runs */
static void
ra_watch(int signed_out)
{
   if (getenv("ROBLOX_NO_ACCOUNT_KEEP"))
      return;
   if (signed_out) {
      if (remove(RA_BACKUP) == 0)
         report("roblox: accounts: signed out: kept account list dropped");
      return;
   }
   long n = 0, bn = 0;
   char *buf = ra_read_file(RA_STORAGE, &n);
   char *bak = ra_read_file(RA_BACKUP, &bn);
   size_t cur = 0;
   char *v = ra_find_blob(buf, &cur);
   if (bak)
      bak[strcspn(bak, "\r\n\"")] = 0;
   const size_t blen = bak ? strlen(bak) : 0;
   if (v && cur > 0 && (cur > blen || (cur == blen && memcmp(v, bak, cur) != 0))) {
      FILE *o = fopen(RA_BACKUP ".new", "wb");
      if (o) {
         fwrite(v, 1, cur, o);
         fclose(o);
         rename(RA_BACKUP ".new", RA_BACKUP);
         report("roblox: accounts: account list kept (%zu bytes)", cur);
      }
   }
   free(buf);
   free(bak);
}
#endif
