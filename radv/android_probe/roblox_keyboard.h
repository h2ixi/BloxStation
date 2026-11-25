/* Text entry: the PS5 system keyboard for Roblox text boxes.
 *
 * On Android, focusing a TextBox makes the engine call the Java method
 * showKeyboard(long textBox, boolean multiLine, byte[] utf8Text, info); the app
 * (com.roblox.client.RbxKeyboard) shows the soft keyboard and, as the text
 * changes, calls NativeGLInterface.nativePassText(textBox, text, false, cursor)
 * and on Enter nativeReturnPressedFromOnScreenKeyboard(textBox). Here the
 * request opens the system IME dialog instead; when the user confirms, the whole
 * text is passed once and Enter is pressed (sends a chat line, runs a search).
 * Cancelling leaves the box unchanged. The controller is not forwarded to the
 * game while the dialog is open.
 *
 * libSceImeDialog has no header in this SDK (see rk_resolve for how it is
 * reached); the parameter layout below is the
 * PS4 one (OpenOrbis), which the PS5 library is expected to share. Every
 * return code is logged, so a mismatch shows as an error, not silence.
 * ROBLOX_NO_KEYBOARD=1 turns this off. */
#ifndef ROBLOX_KEYBOARD_H
#define ROBLOX_KEYBOARD_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

struct rk_ime_param {
   int32_t user_id;
   int32_t type;                 /* 0 default */
   uint64_t supported_languages; /* 0 all */
   int32_t enter_label;          /* 0 default, 1 send, 2 search, 3 go */
   int32_t input_method;         /* 0 default */
   void *filter;
   uint32_t option;              /* 1 multiline */
   uint32_t max_text_length;
   uint16_t *input_text_buffer;
   float posx, posy;
   int32_t horizontal_alignment; /* 1 centre */
   int32_t vertical_alignment;   /* 1 centre */
   const uint16_t *placeholder;
   const uint16_t *title;
   int8_t reserved[16];
};
_Static_assert(sizeof(struct rk_ime_param) == 96, "IME dialog parameter layout");

struct rk_ime_result {
   int32_t end_status; /* 0 ok, 1 cancelled, 2 aborted */
   int32_t reserved[3];
};

int sceUserServiceGetLoginUserIdList(int user_ids[4]);
static int (*rk_init)(const struct rk_ime_param *, const void *);
static int (*rk_status)(void);
static int (*rk_result)(struct rk_ime_result *);
static int (*rk_term)(void);

int sceSysmoduleLoadModule(uint16_t id);
/* Load-time imports, the fallback when the run-time lookup fails. The PS5
 * binds them at the first call; if the module is not loaded by then they
 * resolve to address 0 (the first version of this crashed that way), so the
 * module is loaded at title start (rk_preload) and the calls run under
 * rk_guard: a fault there returns to the keyboard code instead of ending the
 * title. */
int sceImeDialogInit(const struct rk_ime_param *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(struct rk_ime_result *result);
int sceImeDialogTerm(void);
static __thread sigjmp_buf *rk_guard; /* checked by the fault handler */
static int rk_preload_rc = 1;
static void
rk_preload(void)
{
   rk_preload_rc = sceSysmoduleLoadModule(0x0096);
   report("roblox: keyboard: IME dialog module 0x96 loaded at start -> 0x%x", (unsigned)rk_preload_rc);
}

/* What works on the console (2026-11-13): load sysmodule 0x0096 when the
 * title starts, then call the load-time imports (bound lazily at the first
 * call). sceKernelDlsym by name never found the functions, and loading the
 * module a second time by path came just before an ETXTBSY on it, so
 * neither is tried any more. */
static int
rk_resolve(void)
{
   static int done, ok;
   if (done)
      return ok;
   done = 1;
   if (rk_preload_rc != 0) {
      rk_preload_rc = sceSysmoduleLoadModule(0x0096);
      report("roblox: keyboard: IME dialog module 0x96 -> 0x%x", (unsigned)rk_preload_rc);
   }
   if (rk_preload_rc != 0)
      return 0;
   rk_init = sceImeDialogInit;
   rk_status = sceImeDialogGetStatus;
   rk_result = sceImeDialogGetResult;
   rk_term = sceImeDialogTerm;
   ok = 1;
   return 1;
}

#define RK_MAX 200 /* Roblox's chat limit */
static void *rk_pass_text, *rk_return_pressed; /* engine entry points */
static int rk_active;                           /* dialog open: pad input withheld */
static long long rk_box;
static uint16_t rk_buf[RK_MAX + 1];
static int rk_multiline;

/* UTF-8 -> UTF-16 (BMP; others become '?'), at most cap units, terminated. */
static void
rk_to16(const unsigned char *s, size_t n, uint16_t *out, size_t cap)
{
   size_t o = 0;
   for (size_t i = 0; i < n && o < cap;) {
      unsigned c = s[i];
      if (c < 0x80) {
         out[o++] = (uint16_t)c;
         i++;
      } else if ((c & 0xe0) == 0xc0 && i + 1 < n) {
         out[o++] = (uint16_t)(((c & 0x1f) << 6) | (s[i + 1] & 0x3f));
         i += 2;
      } else if ((c & 0xf0) == 0xe0 && i + 2 < n) {
         out[o++] = (uint16_t)(((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f));
         i += 3;
      } else {
         out[o++] = '?';
         i += (c & 0xf8) == 0xf0 ? 4 : 1;
      }
   }
   out[o] = 0;
}

/* UTF-16 (with surrogate pairs) -> UTF-8; returns the number of characters
 * (code points), the cursor position the engine expects at the end. */
static int
rk_to8(const uint16_t *s, char *out, size_t cap)
{
   size_t o = 0;
   int chars = 0;
   for (size_t i = 0; s[i] && o + 4 < cap; i++, chars++) {
      uint32_t c = s[i];
      if (c >= 0xd800 && c < 0xdc00 && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
         c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
         i++;
      }
      if (c < 0x80) {
         out[o++] = (char)c;
      } else if (c < 0x800) {
         out[o++] = (char)(0xc0 | (c >> 6));
         out[o++] = (char)(0x80 | (c & 0x3f));
      } else if (c < 0x10000) {
         out[o++] = (char)(0xe0 | (c >> 12));
         out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
         out[o++] = (char)(0x80 | (c & 0x3f));
      } else {
         out[o++] = (char)(0xf0 | (c >> 18));
         out[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
         out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
         out[o++] = (char)(0x80 | (c & 0x3f));
      }
   }
   out[o] = 0;
   return chars;
}

#if defined(__PROSPERO__)
static void *
rk_thread(void *arg)
{
   (void)arg;
   static const uint16_t title[] = {'R', 'o', 'b', 'l', 'o', 'x', 0};
   struct rk_ime_param p;
   memset(&p, 0, sizeof(p));
   if (!rk_resolve()) {
      __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
      return NULL;
   }
   int users[4] = {-1, -1, -1, -1};
   const int urc = sceUserServiceGetLoginUserIdList(users);
   int user = users[0];
   p.user_id = user;
   p.enter_label = rk_multiline ? 0 : 1;
   p.option = rk_multiline ? 1u : 0u;
   p.max_text_length = RK_MAX;
   p.input_text_buffer = rk_buf;
   p.posx = 960.0f;
   p.posy = 540.0f;
   p.horizontal_alignment = 1;
   p.vertical_alignment = 1;
   p.title = title;
   static int broken;
   sigjmp_buf jb;
   if (broken || sigsetjmp(jb, 1)) {
      rk_guard = NULL;
      if (!broken)
         report("roblox: keyboard: the IME dialog functions are not callable (fault caught); keyboard off");
      broken = 1;
      __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
      return NULL;
   }
   rk_guard = &jb;
   const int rc = rk_init(&p, NULL);
   report("roblox: keyboard: IME dialog for text box %llx (user %d, 0x%x) -> 0x%x", rk_box, user, (unsigned)urc,
          (unsigned)rc);
   if (rc < 0) {
      rk_guard = NULL;
      __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
      return NULL;
   }
   int st;
   while ((st = rk_status()) == 1) /* running */
      usleep(50 * 1000);
   struct rk_ime_result r;
   memset(&r, 0, sizeof(r));
   const int grc = rk_result(&r);
   rk_term();
   rk_guard = NULL;
   report("roblox: keyboard: status %d, result 0x%x, end %d", st, (unsigned)grc, r.end_status);
   if (grc >= 0 && r.end_status == 0 && rk_pass_text) {
      void *env = NULL;
      fj_AttachCurrentThread(&fj_the_vm, (fj_env **)&env, NULL);
      void *cls = fj_class("com/roblox/engine/jni/NativeGLInterface");
      static char text[RK_MAX * 4 + 8];
      const int chars = rk_to8(rk_buf, text, sizeof(text));
      void (*pass)(void *, void *, long long, void *, unsigned char, int) =
         (void (*)(void *, void *, long long, void *, unsigned char, int))rk_pass_text;
      /* The engine's nativePassText returns at once unless its flag is set
       * (disassembly: test r8d; je ret), and ignores the box and cursor. So
       * the text goes in with the flag set while the box still has focus,
       * then Return. */
      pass(env, cls, rk_box, fj_NewStringUTF(env, text), 1, chars);
      usleep(150 * 1000); /* let the engine apply it before Enter */
      if (rk_return_pressed && !rk_multiline)
         ((void (*)(void *, void *, long long))rk_return_pressed)(env, cls, rk_box);
      report("roblox: keyboard: passed %d characters and Enter", chars);
   }
   __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
   return NULL;
}
#endif

/* The engine's showKeyboard/hideKeyboard Java calls. Two classes forward the
 * same request (NativeGLJavaInterface and NativeHelper.gameActivity_*); only
 * one dialog opens. */
static int
rk_java_hook(const char *key, const char *sig, const uint64_t *args, uint64_t *result)
{
   (void)sig;
   const int show = strstr(key, "showKeyboard") != NULL, hide = strstr(key, "hideKeyboard") != NULL;
   if (!show && !hide)
      return 0;
   static int off = -1;
   if (off < 0)
      off = getenv("ROBLOX_NO_KEYBOARD") != NULL;
   *result = 0;
   if (off)
      return 1;
#if defined(__PROSPERO__)
   if (hide)
      return 1; /* the dialog closes itself; it is the user's to finish */
   if (__atomic_exchange_n(&rk_active, 1, __ATOMIC_ACQ_REL))
      return 1; /* already open */
   rk_box = (long long)args[0];
   rk_multiline = (int)(args[1] & 0xff);
   const struct fj_obj *a = (const struct fj_obj *)(uintptr_t)args[2];
   if (a && a->magic == FJ_MAGIC && a->kind == FJ_ARRAY && a->data)
      rk_to16(a->data, (size_t)a->length, rk_buf, RK_MAX);
   else
      rk_buf[0] = 0;
   pthread_t t;
   if (pthread_create(&t, NULL, rk_thread, NULL) == 0)
      pthread_detach(t);
   else
      __atomic_store_n(&rk_active, 0, __ATOMIC_RELEASE);
#else
   (void)args;
#endif
   return 1;
}

#endif
