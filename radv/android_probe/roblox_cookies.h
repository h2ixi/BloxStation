/* Keeping the login across restarts, with no PC.
 *
 * The engine reports every cookie it receives to the Android app through
 * com/roblox/universalapp/cookie/CookieProtocol.setCookie(String url,
 * String setCookieHeader); the app's cookie store keeps them, and at the
 * next start hands them back with nativeSetMultipleCookies (radv_smoke.c
 * reads /data/roblox/files/session.txt for that). Here the roblox.com
 * cookies are kept in a small jar and written to that same file, so signing
 * in on the PS5 (or switching accounts there) survives a restart; a cookie
 * set expired or empty (sign-out) is removed. Values are credentials: only
 * names and lengths are logged. */
#ifndef ROBLOX_COOKIES_H
#define ROBLOX_COOKIES_H
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifndef RC_SESSION
#define RC_SESSION "/data/roblox/files/session.txt"
#endif
#define RC_MAX 32
static struct {
   char name[64];
   char value[4096];
} rc_jar[RC_MAX];
static unsigned rc_n;
static pthread_mutex_t rc_lock = PTHREAD_MUTEX_INITIALIZER;

/* only these matter for staying signed in; the rest is per-session noise */
static int
rc_wanted(const char *name)
{
   return strcmp(name, ".ROBLOSECURITY") == 0 || strcmp(name, "RBXSessionTracker") == 0 ||
          strcmp(name, "RBXEventTrackerV2") == 0 || strcmp(name, "GuestData") == 0 ||
          strcmp(name, ".RBXID") == 0 ||
          /* the account switcher's own cookie: without it a restart left only
           * the current account usable in "Switch accounts" */
          strcmp(name, "rbxas") == 0;
}

/* Load what the title handed to the engine at start, so the jar starts
 * with it ("name=value; name2=value2"). */
static void
rc_load(void)
{
   FILE *f = fopen(RC_SESSION, "rb");
   if (!f)
      return;
   static char buf[16384];
   const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = 0;
   char *save = NULL;
   for (char *tok = strtok_r(buf, ";", &save); tok && rc_n < RC_MAX; tok = strtok_r(NULL, ";", &save)) {
      while (*tok == ' ')
         tok++;
      char *eq = strchr(tok, '=');
      if (!eq)
         continue;
      *eq = 0;
      char *v = eq + 1;
      size_t vl = strlen(v);
      while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r' || v[vl - 1] == ' '))
         v[--vl] = 0;
      if (!rc_wanted(tok))
         continue;
      snprintf(rc_jar[rc_n].name, sizeof(rc_jar[rc_n].name), "%s", tok);
      snprintf(rc_jar[rc_n].value, sizeof(rc_jar[rc_n].value), "%s", v);
      rc_n++;
   }
   memset(buf, 0, sizeof(buf));
}

static void
rc_save(void)
{
   if (!rc_n) {
      remove(RC_SESSION);
      return;
   }
   FILE *f = fopen(RC_SESSION ".new", "wb");
   if (!f)
      return;
   for (unsigned i = 0; i < rc_n; i++)
      fprintf(f, "%s%s=%s", i ? "; " : "", rc_jar[i].name, rc_jar[i].value);
   fclose(f);
   rename(RC_SESSION ".new", RC_SESSION);
}

/* "Set-Cookie" style: name=value; Domain=...; Expires=...; Max-Age=... */
static void
rc_set(const char *header)
{
   char name[64] = "";
   const char *eq = strchr(header, '=');
   if (!eq || (size_t)(eq - header) >= sizeof(name))
      return;
   memcpy(name, header, (size_t)(eq - header));
   name[eq - header] = 0;
   for (char *p = name; *p == ' ';)
      memmove(p, p + 1, strlen(p));
   if (!rc_wanted(name))
      return;
   const char *v = eq + 1;
   const char *end = strchr(v, ';');
   const size_t vl = end ? (size_t)(end - v) : strlen(v);
   int gone = vl == 0;
   for (const char *a = end; a && *a; a++) {
      if (strncasecmp(a, "max-age=0", 9) == 0 || strncasecmp(a, "max-age=-", 9) == 0 ||
          strncasecmp(a, "expires=Thu, 01 Jan 1970", 24) == 0 || strncasecmp(a, "expires=Thu, 01-Jan-1970", 24) == 0)
         gone = 1;
   }
   pthread_mutex_lock(&rc_lock);
   unsigned i;
   for (i = 0; i < rc_n; i++)
      if (strcmp(rc_jar[i].name, name) == 0)
         break;
   if (gone) {
      if (i < rc_n) {
         rc_jar[i] = rc_jar[--rc_n];
         report("roblox: login: cookie %s removed (signed out)", name);
         rc_save();
      }
   } else if (vl < sizeof(rc_jar[0].value)) {
      if (i == rc_n && rc_n < RC_MAX)
         rc_n++;
      if (i < RC_MAX) {
         const int changed = strncmp(rc_jar[i].value, v, vl) != 0 || rc_jar[i].value[vl] != 0;
         snprintf(rc_jar[i].name, sizeof(rc_jar[i].name), "%s", name);
         memcpy(rc_jar[i].value, v, vl);
         rc_jar[i].value[vl] = 0;
         if (changed) {
            report("roblox: login: cookie %s saved (%zu bytes)", name, vl);
            rc_save();
         }
      }
   }
   pthread_mutex_unlock(&rc_lock);
}

/* The account switcher (and some sign-ins) set the login inside the engine
 * without a CookieProtocol.setCookie call, so the jar kept the previous
 * account and a restart brought it back. After each DID_LOG_IN /
 * DID_SWITCH_ACCOUNT the engine's own cookies for roblox.com
 * (NativeSettingsInterface.nativeGetCookiesForDomain, "a=b; c=d") are read
 * and the login cookies saved. */
static void
rc_sync_from_engine(void *env, void *(*get)(void *, void *, void *))
{
   if (!get)
      return;
   void *cls = fj_class("com/roblox/engine/jni/NativeSettingsInterface");
   /* the argument's form is not documented: try a few */
   static const char *const forms[] = {"roblox.com", ".roblox.com", "www.roblox.com", "https://www.roblox.com/"};
   const char *all = NULL;
   for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
      void *res = get(env, cls, fj_NewStringUTF(env, forms[f]));
      const char *t = res ? fj_GetStringUTFChars(NULL, res, NULL) : NULL;
      {
         /* names only: values are credentials */
         char names[200] = "";
         const char *q = t;
         while (q && *q && strlen(names) < 150) {
            while (*q == ' ' || *q == ';' || *q == '\t' || *q == '\n')
               q++;
            const char *e = q;
            while (*e && *e != '=' && *e != ';' && *e != '\t' && *e != '\n')
               e++;
            if (e > q && e - q < 40)
               snprintf(names + strlen(names), sizeof(names) - strlen(names), "%.*s ", (int)(e - q), q);
            q = strpbrk(e, ";\n");
         }
         report("roblox: login: cookies for \"%s\": %zu bytes, names: %s", forms[f], t ? strlen(t) : 0, names);
      }
      if (t && strstr(t, ".ROBLOSECURITY")) {
         all = t;
         report("roblox: login: engine cookies read for \"%s\"", forms[f]);
         break;
      }
   }
   if (!all || !*all) {
      report("roblox: login: engine has no roblox.com cookies to save");
      return;
   }
   /* Netscape cookie-file lines: domain, subdomains, path, secure,
    * expiry, name, value (tab separated; "#HttpOnly_" prefix allowed) */
   static char buf[16384];
   snprintf(buf, sizeof(buf), "%s", all);
   unsigned seen = 0;
   pthread_mutex_lock(&rc_lock);
   static int loaded;
   if (!loaded) {
      loaded = 1;
      rc_load();
   }
   pthread_mutex_unlock(&rc_lock);
   char *line_save = NULL;
   for (char *line = strtok_r(buf, "\n;", &line_save); line; line = strtok_r(NULL, "\n;", &line_save)) {
      while (*line == ' ')
         line++;
      char *f[8];
      int nf = 0;
      for (char *p = line; nf < 8;) {
         f[nf++] = p;
         char *t = strchr(p, '\t');
         if (!t)
            break;
         *t = 0;
         p = t + 1;
      }
      {
         static int said;
         if (said++ < 12)
            report("roblox: login: engine cookie line: %d fields, name %s", nf, nf > 5 ? f[5] : "?");
      }
      if (nf < 7 || !rc_wanted(f[5]))
         continue;
      char *v = f[6];
      v[strcspn(v, "\r")] = 0;
      if (!*v)
         continue;
      static char hdr[4200];
      snprintf(hdr, sizeof(hdr), "%s=%s", f[5], v);
      rc_set(hdr); /* saves session.txt when the value changed */
      seen++;
   }
   report("roblox: login: %u login cookies read back from the engine", seen);
}

static int
rc_java_hook(const char *key, const char *sig, const uint64_t *args, uint64_t *result)
{
   (void)sig;
   if (strcmp(key, "com/roblox/universalapp/cookie/CookieProtocol.setCookie") != 0)
      return 0;
   *result = 0;
   static int loaded;
   if (!loaded) {
      loaded = 1;
      pthread_mutex_lock(&rc_lock);
      rc_load();
      pthread_mutex_unlock(&rc_lock);
   }
   const char *url = fj_GetStringUTFChars(NULL, (void *)(uintptr_t)args[0], NULL);
   const char *hdr = fj_GetStringUTFChars(NULL, (void *)(uintptr_t)args[1], NULL);
   if (url && hdr && strstr(url, "roblox.com"))
      rc_set(hdr);
   return 1;
}

#endif
