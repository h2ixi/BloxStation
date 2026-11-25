/* Host test for roblox_cookies.h: the login cookie jar.
 * cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *    -o /tmp/cookies-test radv/android_probe/cookies_test.c && /tmp/cookies-test */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void report(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
static const char *fj_GetStringUTFChars(void *e, void *s, unsigned char *c) { (void)e; (void)c; return s; }
static void *fj_class(const char *n) { return (void *)n; }
static void *fj_NewStringUTF(void *e, const char *s) { (void)e; return (void *)s; }
#define RC_SESSION "/tmp/cookies-test-session.txt"
#include "roblox_cookies.h"

static char *slurp(void)
{
   static char b[16384];
   FILE *f = fopen(RC_SESSION, "rb");
   if (!f) return NULL;
   b[fread(b, 1, sizeof(b) - 1, f)] = 0;
   fclose(f);
   return b;
}
static void set(const char *url, const char *hdr)
{
   uint64_t args[2] = {(uint64_t)(uintptr_t)url, (uint64_t)(uintptr_t)hdr}, r;
   assert(rc_java_hook("com/roblox/universalapp/cookie/CookieProtocol.setCookie", "", args, &r) == 1);
}
static void *engine_cookies(void *env, void *cls, void *form)
{
   (void)env; (void)cls; (void)form;
   return (void *)".roblox.com\tTRUE\t/\tTRUE\t0\t.ROBLOSECURITY\tSYNCED_TEST_VALUE\n";
}
int main(void)
{
   remove(RC_SESSION);
   FILE *f = fopen(RC_SESSION, "w"); /* what the title loaded at start */
   fputs(".ROBLOSECURITY=OLDVALUE; RBXEventTrackerV2=ev1\n", f);
   fclose(f);
   uint64_t r, other[2] = {0, 0};
   assert(rc_java_hook("some/Other.method", "", other, &r) == 0);
   set("https://www.roblox.com/", "Unrelated=1; path=/");                 /* ignored */
   assert(strcmp(slurp(), ".ROBLOSECURITY=OLDVALUE; RBXEventTrackerV2=ev1\n") == 0); /* untouched */
   set("https://www.roblox.com/", ".ROBLOSECURITY=NEW_TEST_VALUE; domain=.roblox.com; HttpOnly; Secure");
   assert(strstr(slurp(), ".ROBLOSECURITY=NEW_TEST_VALUE") && strstr(slurp(), "RBXEventTrackerV2=ev1"));
   assert(!strstr(slurp(), "OLDVALUE") && !strstr(slurp(), "Unrelated"));
   set("https://evil.example/", ".ROBLOSECURITY=STOLEN");                 /* other sites ignored */
   assert(!strstr(slurp(), "STOLEN"));
   set("https://www.roblox.com/", ".ROBLOSECURITY=; expires=Thu, 01 Jan 1970 00:00:00 GMT"); /* sign out */
   assert(!strstr(slurp(), "ROBLOSECURITY") && strstr(slurp(), "RBXEventTrackerV2=ev1"));
   set("https://www.roblox.com/", "RBXEventTrackerV2=ev1; Max-Age=0");
   assert(slurp() == NULL); /* nothing left: the file is removed */
   /* engine sync (account switch): Netscape cookie lines from the engine */
   rc_sync_from_engine(NULL, engine_cookies);
   assert(slurp() && strstr(slurp(), ".ROBLOSECURITY=SYNCED_TEST_VALUE"));
   puts("PASS: load, save, update, ignore other cookies and sites, sign-out removal, engine sync");
   return 0;
}
