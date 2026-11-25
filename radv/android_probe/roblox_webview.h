/* Web pages (Settings sub-pages, Profile, Messages, ...) in the PS5's own
 * browser.
 *
 * Android shows these in a WebView: its WebViewProtocol subscribes to the
 * app's MessageBus messages "OpenWindow" / "MutateWindow" / "CloseWindow"
 * and answers the "IsAvailable" request with available=true. Here the same
 * subscriptions are made with callback objects of our own classes; the
 * engine calls their run(String) and the URL is opened with
 * sceSystemServiceLaunchWebBrowser. ROBLOX_NO_WEBVIEW=1 turns it off. */
#ifndef ROBLOX_WEBVIEW_H
#define ROBLOX_WEBVIEW_H
#include <stdio.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);

static char rw_url_key[64] = "url", rw_avail_key[64] = "isAvailable";
static unsigned rw_opens;

/* a JSON string member, unescaped enough for URLs ("\/" -> "/") */
static int
rw_json_str(const char *json, const char *key, char *out, size_t cap)
{
   char pat[96];
   snprintf(pat, sizeof(pat), "\"%s\"", key);
   const char *p = strstr(json, pat);
   if (!p)
      return 0;
   p = strchr(p + strlen(pat), ':');
   if (!p)
      return 0;
   while (*++p == ' ')
      ;
   if (*p != '"')
      return 0;
   size_t n = 0;
   for (p++; *p && *p != '"' && n + 1 < cap; p++) {
      if (*p == '\\' && p[1]) {
         p++;
         if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) { /* & etc.: ASCII only */
            unsigned v = 0;
            sscanf(p + 1, "%4x", &v);
            out[n++] = v < 128 ? (char)v : '?';
            p += 4;
            continue;
         }
      }
      out[n++] = *p;
   }
   out[n] = 0;
   return n > 0;
}

/* The in-game browser (libSceWebBrowserDialog): the page opens over Roblox
 * inside the title, as on Android, instead of switching to the PS5's Web
 * Browser app. Layout of the parameter block: SharpProspero / PS4Mount
 * (328 bytes; the magic is 0xC0D1A109 plus the base block's address). */
struct rw_dialog_param {
   uint64_t base_size;
   uint8_t base_reserved[36];
   uint32_t base_magic;
   uint64_t size;
   int32_t mode; /* 1 default, 2 custom */
   int32_t user_id;
   const char *url;
   void *callback;
   uint16_t width, height, x, y;
   uint32_t parts;
   uint16_t header_width, header_x, header_y, pad0;
   uint32_t control;
   void *ime, *webview;
   uint32_t animation;
   uint8_t reserved[202];
   uint16_t pad1;
};
_Static_assert(sizeof(struct rw_dialog_param) == 328, "SceWebBrowserDialogParam is 328 bytes");
int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(const void *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);
int sceCommonDialogInitialize(void);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t *start_out, size_t *size_out);
size_t sceKernelGetDirectMemorySize(void);

static int rw_dialog_rc = 1; /* sysmodule 0xAB load at title start */
/* after the browser closes, tell the app (as Android's WebView does with
 * the protocol's HandleWindowClose message), or its page stays blank */
static void *(*rw_publish)(void *, void *, void *, void *);
static void *rw_bus_cls, *rw_close_msg;
static int rw_closed_notify;

/* from the pad thread: the browser closed -> HandleWindowClose to the app */
/* a roblox:// link from a page (Join on a server list, ...): after the
 * browser closes it goes to the app's link handling (LinkingProtocol
 * HandleLuaURL), as Android hands such links from its WebView to the app */
static void *rw_lua_url_msg, *rw_engine_url_msg;
/* the engine's registered game-start pattern (as the app registers it, JSON
 * escaped; watching registerURL live crashed the in-game browser) */
static char rw_game_pattern[600] =
   "(?:(?:https?://(?:www\\\\.)?\\\\w+\\\\.roblox(?:labs)?\\\\.com(?:/[A-Za-z]{2}(-[A-Za-z0-9]{2,3})?)?/games/start"
   "\\\\?)|(?:roblox(?:mobile)?://(?:experiences/start\\\\?)?))(?:(?:(?:\\\\w+=(?:.+))&?)+)/?";
static int rw_link_engine;
static char rw_link_key[64] = "url", rw_link_matched_key[64] = "matchedUrl";
static char rw_link[1100];
static int rw_link_pending;

static void
rw_pad_tick(void *env)
{
   if (!__atomic_exchange_n(&rw_closed_notify, 0, __ATOMIC_ACQ_REL) || !rw_publish || !rw_close_msg)
      return;
   rw_publish(env, rw_bus_cls, rw_close_msg, fj_NewStringUTF(env, "{}"));
   report("roblox: web: told the app the page was closed");
   if (__atomic_exchange_n(&rw_link_pending, 0, __ATOMIC_ACQ_REL)) {
      /* game starts are the engine's (it registered that pattern), the rest
       * the menus' */
      void *msg = rw_link_engine && rw_engine_url_msg ? rw_engine_url_msg : rw_lua_url_msg;
      if (msg) {
         char json[3200];
         snprintf(json, sizeof(json), "{\"%s\":\"%s\",\"%s\":\"%s\"}", rw_link_key, rw_link, rw_link_matched_key,
                  rw_link_engine && rw_game_pattern[0] ? rw_game_pattern : rw_link);
         rw_publish(env, rw_bus_cls, msg, fj_NewStringUTF(env, json));
         report("roblox: web: link to the %s: %.300s", msg == rw_engine_url_msg ? "engine" : "menus", json);
      }
   }
}
static int rw_dialog_open, rw_close_req; /* the pad thread asks for a close (Circle) */
/* Roblox's frames wait while the browser is up (roblox_vulkan.h): the
 * browser (CE-108262-9) crashed at start-up, most often after a game, with
 * our GPU driver busy at 120 fps; the page covers the screen anyway */
static int rw_render_pause;
static char rw_dialog_url[2300];
static struct rw_dialog_param rw_dp __attribute__((aligned(16)));

static void
rw_preload(void)
{
#if defined(__PROSPERO__)
   rw_dialog_rc = sceSysmoduleLoadModule(0x00AB);
   /* the common dialog system once, at start: initialising it right before
    * the first open is when the browser crashed (CE-108262-9) */
   const int c = rw_dialog_rc == 0 ? sceCommonDialogInitialize() : -1;
   report("roblox: web: in-game browser module 0xab loaded at start -> 0x%x, common dialog init -> 0x%x",
          (unsigned)rw_dialog_rc, (unsigned)c);
#endif
}

#if defined(__PROSPERO__)
static void *
rw_dialog_thread(void *arg)
{
   (void)arg;
   sigjmp_buf jb;
   int ok = 0;
   if (sigsetjmp(jb, 1) == 0) {
      rk_guard = &jb;
      /* each page: initialise, open, run, close, terminate (a second open
       * without terminate fails with 0x80b8000e) */
      const int c = sceCommonDialogInitialize();
      const int w = sceWebBrowserDialogInitialize();
      report("roblox: web: common dialog init -> 0x%x, browser dialog init -> 0x%x", (unsigned)c, (unsigned)w);
      const int inited = w >= 0;
      usleep(300 * 1000); /* let it settle before the open */
      if (inited) {
         int users[4] = {-1, -1, -1, -1};
         sceUserServiceGetLoginUserIdList(users);
         memset(&rw_dp, 0, sizeof(rw_dp));
         rw_dp.base_size = 48;
         rw_dp.base_magic = (uint32_t)(0xC0D1A109u + (uint64_t)(uintptr_t)&rw_dp);
         rw_dp.size = sizeof(rw_dp);
         rw_dp.user_id = users[0];
         rw_dp.url = rw_dialog_url;
         /* custom layout, full screen, no browser frame: the page is the
          * screen, as in the Android app (mode 1 is the framed standard one) */
         rw_dp.mode = 2;
         rw_dp.width = 1920, rw_dp.height = 1080, rw_dp.x = 0, rw_dp.y = 0;
         rw_dp.header_width = 1920;
         rw_dp.control = 0x20; /* Circle closes it (without this nothing does) */
         { /* memory at the open: the browser has crashed at start-up (CE-108262-9) */
            size_t flex = 0, dsize = 0;
            off_t dstart = 0;
            sceKernelAvailableFlexibleMemorySize(&flex);
            sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), 0, &dstart, &dsize);
            report("roblox: web: memory before the browser: flexible %zu MiB free, direct %zu MiB free (largest)",
                   flex >> 20, dsize >> 20);
         }
         __atomic_store_n(&rw_render_pause, 1, __ATOMIC_RELEASE);
         usleep(250 * 1000); /* the frames in flight finish */
         const int o = sceWebBrowserDialogOpen(&rw_dp);
         report("roblox: web: in-game browser open (mode %d, parts 0x%x, control 0x%x, %ux%u at %u,%u, header %u, "
                "user %d) -> 0x%x",
                rw_dp.mode, rw_dp.parts, rw_dp.control, rw_dp.width, rw_dp.height, rw_dp.x, rw_dp.y,
                rw_dp.header_width, rw_dp.user_id, (unsigned)o);
         if (o == 0) {
            ok = 1;
            int st, polls = 0;
            __atomic_store_n(&rw_close_req, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&rw_dialog_open, 1, __ATOMIC_RELEASE);
            while ((st = sceWebBrowserDialogUpdateStatus()) == 2 || st == 1) {
               usleep(16000);
               if ((polls & 31) == 0 && access("/data/roblox/files/web-close", F_OK) == 0) { /* recovery: close a stuck page over FTP */
                  unlink("/data/roblox/files/web-close");
                  __atomic_store_n(&rw_close_req, 1, __ATOMIC_RELEASE);
               }
               if (__atomic_exchange_n(&rw_close_req, 0, __ATOMIC_ACQ_REL)) {
                  report("roblox: web: Circle: closing the in-game browser");
                  sceWebBrowserDialogClose();
               }
               if (++polls == 1)
                  report("roblox: web: in-game browser running");
            }
            __atomic_store_n(&rw_dialog_open, 0, __ATOMIC_RELEASE);
            report("roblox: web: in-game browser closed (status %d after %d polls)", st, polls);
            sceWebBrowserDialogClose();
         }
         sceWebBrowserDialogTerminate();
      }
   } else {
      report("roblox: web: in-game browser call faulted");
   }
   rk_guard = NULL;
   __atomic_store_n(&rw_render_pause, 0, __ATOMIC_RELEASE);
   if (!ok)
      report("roblox: web: the in-game browser did not open");
   /* the engine's JNI helpers need a thread it knows: the pad thread
    * sends it (rw_pad_tick) */
   __atomic_store_n(&rw_closed_notify, 1, __ATOMIC_RELEASE);
   __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
   return NULL;
}
#endif

static void
rw_open(const char *url)
{
   rw_opens++;
   /* roblox.com is blocked on the console's network: through the relay */
   static char via[2300];
   web_relay_url(url, via, sizeof(via));
#if defined(__PROSPERO__)
   if (rw_dialog_rc == 0) {
      if (__atomic_exchange_n(&rk_active, 1, __ATOMIC_ACQ_REL)) {
         report("roblox: web: a system dialog is already open; %s not opened", via);
         return;
      }
      snprintf(rw_dialog_url, sizeof(rw_dialog_url), "%s", via);
      report("roblox: web: opening %s in the in-game browser", via);
      pthread_t t;
      if (pthread_create(&t, NULL, rw_dialog_thread, NULL) == 0) {
         pthread_detach(t);
         return;
      }
      __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
   }
#endif
   report("roblox: web: opening %s in the PS5 browser", via);
   const int r = sceSystemServiceLaunchWebBrowser(via, NULL);
   report("roblox: web: sceSystemServiceLaunchWebBrowser -> 0x%08x", (unsigned)r);
}

/* from the relay (any thread): robloxmobile://placeID=1&gameInstanceId=2
 * -> roblox://experiences/start?placeId=1&gameInstanceId=2; roblox://... as is */
static void
rw_app_link(const char *in)
{
   char u[1100];
   size_t n = 0;
   for (const char *p = in; *p && n + 1 < sizeof(u); p++) { /* %xx */
      unsigned v;
      if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) {
         u[n++] = (char)v;
         p += 2;
      } else {
         u[n++] = *p;
      }
   }
   u[n] = 0;
   if (strcmp(u, "ps5://close") == 0) { /* the page's Circle on its first page (web_relay.c) */
      report("roblox: web: page closed the browser (Circle)");
      __atomic_store_n(&rw_close_req, 1, __ATOMIC_RELEASE);
      return;
   }
   for (char *q = u; *q; q++)
      if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20)
         *q = '_'; /* it goes into JSON */
   snprintf(rw_link, sizeof(rw_link), "%s", u);
   /* game starts: robloxmobile://placeID=..., roblox://experiences/start?...,
    * roblox://placeId=... (the engine's pattern) */
   rw_link_engine = strncmp(u, "robloxmobile://", 15) == 0 || strncmp(u, "roblox://experiences/start?", 27) == 0 ||
                    (strncmp(u, "roblox://", 9) == 0 && strchr(u + 9, '=') && !strchr(u + 9, '/'));
   report("roblox: web: page opened %s -> %s", in, rw_link);
   __atomic_store_n(&rw_link_pending, 1, __ATOMIC_RELEASE);
   __atomic_store_n(&rw_close_req, 1, __ATOMIC_RELEASE); /* close the browser */
}

static int
rw_java_hook(const char *key, const char *sig, const uint64_t *args, uint64_t *result)
{
   (void)sig;
   if (strncmp(key, "ps5/Web", 7) != 0)
      return 0;
   *result = 0;
   const char *json = fj_GetStringUTFChars(NULL, (void *)(uintptr_t)args[0], NULL);
   if (!json)
      json = "";
   if (strncmp(key, "ps5/WebAvail.", 13) == 0) {
      char ans[128];
      snprintf(ans, sizeof(ans), "{\"%s\":true}", rw_avail_key);
      *result = (uint64_t)(uintptr_t)fj_NewStringUTF(NULL, ans);
      static int said;
      if (!said++)
         report("roblox: web: told the app web pages are available");
      return 1;
   }
   static unsigned logged;
   if (logged++ < 20)
      report("roblox: web: %s %.300s", key + 4, json);
   if (strncmp(key, "ps5/WebOpen.", 12) == 0) {
      char url[2048];
      if (rw_json_str(json, rw_url_key, url, sizeof(url))) {
         /* The "Guide" item in the left menu (tools/patch-guide-menu.sh) is
          * Roblox's Build item pointed at the BuildersClub page, whose only
          * action is this page (More > Roblox Plus opens /plus). It opens the
          * on-screen guide (roblox_guide.h) instead of a web page; the app is
          * told the window closed so it does not wait for one. */
         if (strcmp(url, "https://www.roblox.com/premium/membership") == 0) {
            report("roblox: web: left menu > Guide");
            rg_open(0);
            __atomic_store_n(&rw_closed_notify, 1, __ATOMIC_RELEASE);
         } else {
            rw_open(url);
         }
      }
   }
   return 1;
}

/* the subscriptions, once the engine is up (as WebViewProtocol's constructor) */
static void
rw_setup(void *lib, void *env)
{
   if (getenv("ROBLOX_NO_WEBVIEW"))
      return;
#define RW_SYM(n) ael_symbol(lib, "Java_com_roblox_protocols_webview_WebViewProtocol_" n)
   void *(*s_proto)(void *, void *) = RW_SYM("getProtocolName");
   void *(*s_open)(void *, void *) = RW_SYM("getOpenWindowId");
   void *(*s_mut)(void *, void *) = RW_SYM("getMutateWindowId");
   void *(*s_close)(void *, void *) = RW_SYM("getCloseWindowId");
   void *(*s_avail)(void *, void *) = RW_SYM("getIsAvailableId");
   void *(*s_urlkey)(void *, void *) = RW_SYM("getUrlKey");
   void *(*s_availkey)(void *, void *) = RW_SYM("getAvailableKey");
   void (*init)(void *, void *) = RW_SYM("initializeAndroidWebViewProtocol");
#undef RW_SYM
   void *(*msg_id)(void *, void *, void *, void *) =
      ael_symbol(lib, "Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId");
   void *(*sub)(void *, void *, void *, void *, unsigned char) =
      ael_symbol(lib, "Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw");
   void *(*sub_req)(void *, void *, void *, void *, void *, unsigned char) =
      ael_symbol(lib, "Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeProtocolMethodRequestRaw");
   if (!s_proto || !s_open || !msg_id || !sub) {
      report("roblox: web: protocol entry points missing");
      return;
   }
   void *wcls = fj_class("com/roblox/protocols/webview/WebViewProtocol");
   void *bcls = fj_class("com/roblox/universalapp/messagebus/MessageBus");
   struct fj_obj *bus = fj_new(FJ_OBJECT, "MessageBus", NULL);
   bus->cls = bcls;
   if (init)
      init(env, wcls);
   const char *t;
   if (s_urlkey && (t = fj_GetStringUTFChars(env, s_urlkey(env, wcls), NULL)))
      snprintf(rw_url_key, sizeof(rw_url_key), "%s", t);
   if (s_availkey && (t = fj_GetStringUTFChars(env, s_availkey(env, wcls), NULL)))
      snprintf(rw_avail_key, sizeof(rw_avail_key), "%s", t);
   void *proto = s_proto(env, wcls);
   const struct {
      void *(*id)(void *, void *);
      const char *cls;
   } subs[] = {{s_open, "ps5/WebOpen"}, {s_mut, "ps5/WebMutate"}, {s_close, "ps5/WebClose"}};
   for (unsigned i = 0; i < 3; i++) {
      if (!subs[i].id)
         continue;
      void *mid = msg_id(env, bcls, proto, subs[i].id(env, wcls));
      struct fj_obj *cb = fj_new(FJ_OBJECT, subs[i].cls, NULL);
      cb->cls = fj_class(subs[i].cls);
      void *conn = sub(env, bus, mid, cb, 0);
      report("roblox: web: subscribed to %s (%s) -> %p", fj_GetStringUTFChars(env, mid, NULL), subs[i].cls, conn);
   }
   void *(*s_hclose)(void *, void *) = ael_symbol(lib, "Java_com_roblox_protocols_webview_WebViewProtocol_getHandleWindowCloseId");
   rw_publish = ael_symbol(lib, "Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw");
   if (s_hclose && rw_publish) {
      rw_bus_cls = bcls;
      rw_close_msg = msg_id(env, bcls, proto, s_hclose(env, wcls));
      report("roblox: web: window-closed message is %s", fj_GetStringUTFChars(env, rw_close_msg, NULL));
   }
   { /* LinkingProtocol: where links go */
#define RL_SYM(n) ael_symbol(lib, "Java_com_roblox_universalapp_linking_JNILinkingProtocol_" n)
      void *(*l_proto)(void *, void *) = RL_SYM("getProtocolName");
      void *(*l_lua)(void *, void *) = RL_SYM("getHandleLuaURLId");
      void *(*l_urlkey)(void *, void *) = RL_SYM("getUrlKey");
      void *(*l_matched)(void *, void *) = RL_SYM("getMatchedUrlKey");
#undef RL_SYM
      void *lcls = fj_class("com/roblox/universalapp/linking/JNILinkingProtocol");
      if (l_proto && l_lua) {
         void *lproto = l_proto(env, lcls);
         /* some ids already carry the protocol name ("Linking.handleLuaURL"):
          * passing them as is made "Linking.Linking.handleLuaURL" */
         const char *pn = fj_GetStringUTFChars(env, lproto, NULL);
         const size_t pl = pn ? strlen(pn) : 0;
#define RL_ID(getter)                                                                                       \
   ({                                                                                                       \
      void *id_ = (getter)(env, lcls);                                                                      \
      const char *is_ = fj_GetStringUTFChars(env, id_, NULL);                                               \
      (is_ && pl && strncmp(is_, pn, pl) == 0 && is_[pl] == '.') ? fj_NewStringUTF(env, is_ + pl + 1) : id_; \
   })
         rw_lua_url_msg = msg_id(env, bcls, lproto, RL_ID(l_lua));
         void *(*l_engine)(void *, void *) =
            ael_symbol(lib, "Java_com_roblox_universalapp_linking_JNILinkingProtocol_getHandleEngineURLId");
         if (l_engine)
            rw_engine_url_msg = msg_id(env, bcls, lproto, RL_ID(l_engine));
         if (l_urlkey && (t = fj_GetStringUTFChars(env, l_urlkey(env, lcls), NULL)))
            snprintf(rw_link_key, sizeof(rw_link_key), "%s", t);
         if (l_matched && (t = fj_GetStringUTFChars(env, l_matched(env, lcls), NULL)))
            snprintf(rw_link_matched_key, sizeof(rw_link_matched_key), "%s", t);
         report("roblox: web: links go to %s / %s (keys \"%s\", \"%s\")",
                fj_GetStringUTFChars(env, rw_lua_url_msg, NULL),
                rw_engine_url_msg ? fj_GetStringUTFChars(env, rw_engine_url_msg, NULL) : "-", rw_link_key,
                rw_link_matched_key);
#undef RL_ID
      }
   }
   if (sub_req && s_avail) {
      struct fj_obj *cb = fj_new(FJ_OBJECT, "ps5/WebAvail", NULL);
      cb->cls = fj_class("ps5/WebAvail");
      void *conn = sub_req(env, bus, proto, s_avail(env, wcls), cb, 0);
      report("roblox: web: answering IsAvailable -> %p (url key \"%s\", available key \"%s\")", conn, rw_url_key,
             rw_avail_key);
   }
}
#endif
