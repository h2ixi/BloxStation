/*
 * A stand-in Java VM for libroblox.so's JNI entry points on the PS5.
 *
 * There is no Java on the console. This gives the library a JavaVM and a
 * JNIEnv whose function tables follow jni.h's slot order (jni_slots.h): the
 * calls a native library makes to look Java up (FindClass, GetMethodID,
 * RegisterNatives, strings, references) are answered with small handle
 * objects and logged; every other slot is filled by the caller's trap
 * resolver, so the first call to anything not provided yet is logged by name.
 */
#ifndef FAKE_JNI_H
#define FAKE_JNI_H

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jni_slots.h"

#define FJ_ENV_SLOTS (sizeof(jni_env_slot_names) / sizeof(jni_env_slot_names[0]))
#define FJ_VM_SLOTS (sizeof(jni_vm_slot_names) / sizeof(jni_vm_slot_names[0]))

enum { FJ_MAGIC = 0x4a4e4931, FJ_CLASS = 1, FJ_STRING = 2, FJ_OBJECT = 3, FJ_ID = 4, FJ_ARRAY = 5 };

struct fj_obj {
   uint32_t magic;
   int kind;
   const struct fj_obj *cls; /* an object's class */
   char *text;               /* class name, string contents, or member name */
   char *sig;                /* member signature */
   int is_static;
   long length;   /* arrays: element count */
   int elem_size; /* arrays: bytes per element (8 for objects) */
   void *data;    /* arrays: elements */
};

typedef struct {
   const void *const *functions;
} fj_env;

typedef struct {
   const void *const *functions;
} fj_vm;

static void *fj_env_table[FJ_ENV_SLOTS];
static void *fj_vm_table[FJ_VM_SLOTS];
static fj_env fj_the_env = {(const void *const *)fj_env_table};
static fj_vm fj_the_vm = {(const void *const *)fj_vm_table};
static void (*fj_log)(const char *line);
static unsigned fj_classes, fj_methods, fj_natives;

static void
fj_say(const char *fmt, ...)
{
   char line[400];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (fj_log)
      fj_log(line);
}

static char *
fj_strdup(const char *s)
{
   const size_t n = s ? strlen(s) : 0;
   char *d = malloc(n + 1);
   if (d) {
      if (n)
         memcpy(d, s, n);
      d[n] = '\0';
   }
   return d;
}

static struct fj_obj *
fj_new(int kind, const char *text, const char *sig)
{
   struct fj_obj *o = calloc(1, sizeof(*o));
   if (!o)
      return NULL;
   o->magic = FJ_MAGIC;
   o->kind = kind;
   o->text = fj_strdup(text);
   o->sig = sig ? fj_strdup(sig) : NULL;
   return o;
}

static const char *
fj_name(const void *p)
{
   const struct fj_obj *o = p;
   if (!o)
      return "null";
   if (o->magic != FJ_MAGIC)
      return "(foreign)";
   return o->text ? o->text : "?";
}

/* A few classes are looked up again and again: keep one handle per name. */
#define FJ_MAX_CLASSES 512
static struct fj_obj *fj_class_list[FJ_MAX_CLASSES];

static struct fj_obj *
fj_class(const char *name)
{
   for (unsigned i = 0; i < fj_classes; i++)
      if (strcmp(fj_class_list[i]->text, name) == 0)
         return fj_class_list[i];
   struct fj_obj *c = fj_new(FJ_CLASS, name, NULL);
   if (c && fj_classes < FJ_MAX_CLASSES)
      fj_class_list[fj_classes++] = c;
   return c;
}

/* ---- JNIEnv ------------------------------------------------------------- */

static int fj_GetVersion(fj_env *e) { (void)e; return 0x00010006; }

static void *
fj_FindClass(fj_env *e, const char *name)
{
   (void)e;
   struct fj_obj *c = fj_class(name ? name : "?");
   fj_say("jni: FindClass(%s)", name ? name : "NULL");
   return c;
}

static void *
fj_GetObjectClass(fj_env *e, void *obj)
{
   (void)e;
   const struct fj_obj *o = obj;
   if (o && o->magic == FJ_MAGIC && o->cls)
      return (void *)o->cls;
   return fj_class(o && o->magic == FJ_MAGIC && o->kind == FJ_STRING ? "java/lang/String" : "java/lang/Object");
}

static void *
fj_member(void *cls, const char *name, const char *sig, int is_static, const char *what)
{
   struct fj_obj *id = fj_new(FJ_ID, name, sig);
   if (id) {
      id->cls = cls;
      id->is_static = is_static;
   }
   fj_methods++;
   if (fj_methods <= 400)
      fj_say("jni: %s %s.%s %s", what, fj_name(cls), name ? name : "?", sig ? sig : "");
   return id;
}

static void *fj_GetMethodID(fj_env *e, void *c, const char *n, const char *s) { (void)e; return fj_member(c, n, s, 0, "GetMethodID"); }
static void *fj_GetStaticMethodID(fj_env *e, void *c, const char *n, const char *s) { (void)e; return fj_member(c, n, s, 1, "GetStaticMethodID"); }
static void *fj_GetFieldID(fj_env *e, void *c, const char *n, const char *s) { (void)e; return fj_member(c, n, s, 0, "GetFieldID"); }
static void *fj_GetStaticFieldID(fj_env *e, void *c, const char *n, const char *s) { (void)e; return fj_member(c, n, s, 1, "GetStaticFieldID"); }

struct fj_native_method {
   const char *name;
   const char *signature;
   void *fn;
};

#define FJ_MAX_NATIVES 1024
static struct {
   const char *cls, *name, *sig;
   void *fn;
} fj_native_list[FJ_MAX_NATIVES];
static unsigned fj_native_count;

static void *
fj_find_native(const char *name)
{
   for (unsigned i = 0; i < fj_native_count; i++)
      if (strcmp(fj_native_list[i].name, name) == 0)
         return fj_native_list[i].fn;
   return NULL;
}

static int
fj_RegisterNatives(fj_env *e, void *cls, const struct fj_native_method *m, int n)
{
   (void)e;
   fj_natives += (unsigned)(n > 0 ? n : 0);
   fj_say("jni: RegisterNatives(%s, %d methods)", fj_name(cls), n);
   for (int i = 0; i < n; i++) {
      if (fj_native_count < FJ_MAX_NATIVES) {
         fj_native_list[fj_native_count].cls = fj_name(cls);
         fj_native_list[fj_native_count].name = m[i].name;
         fj_native_list[fj_native_count].sig = m[i].signature;
         fj_native_list[fj_native_count].fn = m[i].fn;
         fj_native_count++;
      }
      if (i < 24)
         fj_say("jni:    %s %s", m[i].name ? m[i].name : "?", m[i].signature ? m[i].signature : "");
   }
   return 0;
}

static int fj_UnregisterNatives(fj_env *e, void *c) { (void)e; (void)c; return 0; }
static void *fj_Ref(fj_env *e, void *o) { (void)e; return o; }
static void fj_Unref(fj_env *e, void *o) { (void)e; (void)o; }
static unsigned fj_exception_checks;
static int
fj_ExceptionCheck(fj_env *e)
{
   (void)e;
   fj_exception_checks++;
   if (fj_exception_checks <= 3 || (fj_exception_checks & (fj_exception_checks - 1)) == 0)
      fj_say("jni: ExceptionCheck #%u from %p", fj_exception_checks, __builtin_return_address(0));
   return 0;
}
static void *fj_ExceptionOccurred(fj_env *e) { (void)e; return NULL; }
static void fj_ExceptionVoid(fj_env *e) { (void)e; }
static int fj_PushLocalFrame(fj_env *e, int n) { (void)e; (void)n; return 0; }
static void *fj_PopLocalFrame(fj_env *e, void *r) { (void)e; return r; }
static int fj_EnsureLocalCapacity(fj_env *e, int n) { (void)e; (void)n; return 0; }
static int fj_IsSameObject(fj_env *e, void *a, void *b) { (void)e; return a == b; }
static int fj_GetJavaVM(fj_env *e, fj_vm **vm) { (void)e; *vm = &fj_the_vm; return 0; }

static void *
fj_NewStringUTF(fj_env *e, const char *s)
{
   (void)e;
   struct fj_obj *o = fj_new(FJ_STRING, s ? s : "", NULL);
   if (o)
      o->cls = fj_class("java/lang/String");
   return o;
}

static const char *
fj_GetStringUTFChars(fj_env *e, void *str, unsigned char *is_copy)
{
   (void)e;
   if (is_copy)
      *is_copy = 0;
   const struct fj_obj *o = str;
   return o && o->magic == FJ_MAGIC && o->text ? o->text : "";
}

static void fj_ReleaseStringUTFChars(fj_env *e, void *s, const char *c) { (void)e; (void)s; (void)c; }
static int fj_GetStringUTFLength(fj_env *e, void *s) { return (int)strlen(fj_GetStringUTFChars(e, s, NULL)); }
static int fj_GetStringLength(fj_env *e, void *s) { return fj_GetStringUTFLength(e, s); }

static void
fj_GetStringUTFRegion(fj_env *e, void *s, int start, int len, char *buf)
{
   const char *t = fj_GetStringUTFChars(e, s, NULL);
   const int n = (int)strlen(t);
   if (start < 0 || start > n)
      start = n;
   if (len > n - start)
      len = n - start;
   memcpy(buf, t + start, (size_t)(len > 0 ? len : 0));
   buf[len > 0 ? len : 0] = '\0';
}

static int fj_GetObjectRefType(fj_env *e, void *o) { (void)e; return o ? 2 : 0; /* JNIGlobalRefType */ }

/* ---- calls into "Java" -------------------------------------------------------
 * Every Call*Method* slot comes here. The answer depends on the method's
 * class, name and return type: a small table of what the Android app's Java
 * side would say, otherwise zero / null. Arguments are not read. */

struct fj_answer {
   const char *method; /* "class.name" */
   const char *string; /* for a String return */
   long long value;    /* for a primitive return */
};

static long long fj_now_ms(void);

static const struct fj_answer fj_answers[] = {
   {"com/roblox/engine/jni/locale/NativeLocaleJavaInterface.getLocale", "en-us", 0},
   {"com/roblox/engine/jni/locale/NativeLocaleJavaInterface.getRobloxLocale", "en_us", 0},
   {"com/roblox/engine/jni/locale/NativeLocaleJavaInterface.getGameLocale", "en_us", 0},
   {"com/roblox/engine/jni/user/NativeUserJavaInterface.getUsername", "", 0},
   {"com/roblox/engine/jni/user/NativeUserJavaInterface.getDisplayName", "", 0},
   {"com/roblox/engine/jni/user/NativeUserJavaInterface.getAlternateName", "", 0},
   {"com/roblox/engine/jni/user/NativeUserJavaInterface.getPlatformName", "Android", 0},
   {"com/roblox/engine/jni/user/NativeUserJavaInterface.getTheme", "dark", 0},
   {"com/roblox/engine/jni/autovalue/InitParams.baseURL", "https://www.roblox.com/", 0},
   {"com/roblox/engine/jni/autovalue/InitParams.userAgent", "Mozilla/5.0 (Linux; Android 13; PS5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Mobile Safari/537.36 ROBLOX Android App 2.746.1266 Tablet Hybrid()", 0},
   {"com/roblox/engine/jni/autovalue/InitParams.isTablet", NULL, 1},
   {"com/roblox/engine/jni/autovalue/StartAppParams.selectedTheme", "dark", 0},
   {"com/roblox/engine/jni/autovalue/StartAppParams.username", "", 0},
   {NULL, NULL, 0},
};

static unsigned fj_calls;

static const struct fj_obj *
fj_method_of(void *a, void *b, void *c)
{
   /* Static and virtual calls pass (env, class/object, methodID, ...);
    * non-virtual ones (env, object, class, methodID, ...). */
   const struct fj_obj *ids[3] = {b, c, a};
   for (int i = 0; i < 3; i++)
      if (ids[i] && ((const struct fj_obj *)ids[i])->magic == FJ_MAGIC && ((const struct fj_obj *)ids[i])->kind == FJ_ID)
         return ids[i];
   return NULL;
}

/* A Java method the embedding program implements itself (audio output):
 * returns 1 and sets *result when it handled the call. args: the call's
 * arguments, 64 bits each (ints, then object pointers), at least 4. */
static int (*fj_java_hook)(const char *key, const char *sig, const uint64_t *args, uint64_t *result);

static uint64_t fj_call_args(fj_env *e, void *a, void *b, void *c, const uint64_t *args);
static uint64_t
fj_call(fj_env *e, void *a, void *b, void *c)
{
   return fj_call_args(e, a, b, c, NULL);
}
/* Call<T>Method(env, obj, id, ...) / CallNonvirtual<T>Method(env, obj, cls, id, ...) */
static uint64_t
fj_callN(fj_env *e, void *a, void *b, ...)
{
   va_list ap;
   va_start(ap, b);
   uint64_t v[6];
   for (int i = 0; i < 6; i++)
      v[i] = va_arg(ap, uint64_t);
   va_end(ap);
   const struct fj_obj *m = fj_method_of(a, b, NULL);
   return fj_call_args(e, a, b, (void *)(uintptr_t)v[0], m ? v : v + 1);
}
/* Call<T>MethodV(env, obj, id, va_list) */
static uint64_t
fj_callV(fj_env *e, void *a, void *b, void *c, void *d)
{
   const struct fj_obj *m = fj_method_of(a, b, NULL);
   struct __va_list_tag *src = (struct __va_list_tag *)(m ? c : d);
   uint64_t v[6] = {0};
   if (src) {
      va_list ap; /* va_copy: on x86-64 a va_list is one 24-byte struct */
      memcpy(ap, src, sizeof(ap));
      for (int i = 0; i < 6; i++)
         v[i] = va_arg(ap, uint64_t);
      va_end(ap);
   }
   return fj_call_args(e, a, b, m ? NULL : c, v);
}
/* Call<T>MethodA(env, obj, id, const jvalue *) */
static uint64_t
fj_callA(fj_env *e, void *a, void *b, void *c, void *d)
{
   const struct fj_obj *m = fj_method_of(a, b, NULL);
   const uint64_t *src = (const uint64_t *)(m ? c : d);
   uint64_t v[6] = {0};
   for (int i = 0; src && i < 6; i++)
      v[i] = src[i];
   return fj_call_args(e, a, b, m ? NULL : c, v);
}

static uint64_t
fj_call_args(fj_env *e, void *a, void *b, void *c, const uint64_t *args)
{
   (void)e;
   const struct fj_obj *m = fj_method_of(a, b, c);
   char key[256];
   snprintf(key, sizeof(key), "%s.%s", m && m->cls ? fj_name(m->cls) : "?", m ? m->text : "?");
   const char *sig = m && m->sig ? m->sig : "";
   const char *ret = strrchr(sig, ')');
   ret = ret ? ret + 1 : "V";
   uint64_t result = 0;
   if (fj_java_hook && args && fj_java_hook(key, sig, args, &result))
      return result;
   const struct fj_answer *ans = NULL;
   for (const struct fj_answer *x = fj_answers; x->method; x++)
      if (strcmp(x->method, key) == 0)
         ans = x;
   if (strcmp(key, "com/roblox/universalapp/logging/LoggingProtocol.getProcessTimestamp") == 0)
      result = (uint64_t)fj_now_ms();
   else if (ans && ans->string && strcmp(ret, "Ljava/lang/String;") == 0)
      result = (uint64_t)(uintptr_t)fj_NewStringUTF(e, ans->string);
   else if (ans)
      result = (uint64_t)ans->value;
   else if (strncmp(key, "com/roblox/engine/jni/", 22) == 0 && strcmp(ret, "Ljava/lang/String;") == 0)
      result = (uint64_t)(uintptr_t)fj_NewStringUTF(e, ""); /* the engine's own parameter objects: "" not null */
   else if (ret[0] == 'L' && strncmp(key, "com/roblox", 10) == 0) {
      /* Every Roblox Java class answers with an object of its declared class:
       * the real app returns real (often empty) objects, and the engine copies
       * their contents (e.g. an empty map) instead of crashing on a NULL. */
      char cls[200];
      snprintf(cls, sizeof(cls), "%.*s", (int)(strlen(ret) - 2), ret + 1);
      struct fj_obj *o = fj_new(FJ_OBJECT, cls, NULL);
      o->cls = fj_class(cls);
      result = (uint64_t)(uintptr_t)o;
   }
   fj_calls++;
   /* each method is reported the first time it is called, whenever that is
    * (in a game too), and every call among the first 300 */
   static const void *seen[1024];
   static unsigned nseen;
   int first = 1;
   for (unsigned i = 0; i < nseen; i++)
      if (seen[i] == m) {
         first = 0;
         break;
      }
   if (first && nseen < 1024)
      seen[nseen++] = m;
   if (fj_calls <= 300 || first)
      fj_say("jni: call %s %s -> %s%s", key, sig,
             ret[0] == 'L' || ret[0] == '[' ? (result ? "object" : "null") : ret[0] == 'V' ? "void" : "value",
             first && fj_calls > 300 ? " (first call)" : "");
   return result;
}

static double
fj_call_fp(fj_env *e, void *a, void *b, void *c)
{
   (void)fj_call(e, a, b, c);
   return 0.0;
}

static long long
fj_now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *
fj_impl_for_call(const char *name)
{
   if (strncmp(name, "Call", 4) != 0 || !strstr(name, "Method"))
      return NULL;
   if (strstr(name, "Float") || strstr(name, "Double"))
      return (void *)fj_call_fp;
   const size_t n = strlen(name);
   if (name[n - 1] == 'V')
      return (void *)fj_callV;
   if (name[n - 1] == 'A')
      return (void *)fj_callA;
   return (void *)fj_callN;
}

/* ---- fields the library reads: what the Android app's Java objects hold ---- */
struct fj_field_answer {
   const char *field; /* "class.name" */
   const char *string;
   double value;
};
static const struct fj_field_answer fj_field_answers[] = {
#define DP "com/roblox/engine/jni/model/DeviceParams."
   {DP "osVersion", "33", 0}, /* API level, as in DeviceStaticParams (Integer.toString(SDK_INT)) */
   {DP "deviceName", "PlayStation 5", 0},
   {DP "appVersion", "2.746.1266", 0},
   {DP "country", "US", 0},
   {DP "manufacturer", "Sony", 0},
   {DP "displayResolution", "1920x1080", 0},
   {DP "networkType", "WIFI", 0},
   {DP "displayPhysicalWidthPixels", NULL, 1920},
   {DP "displayPhysicalHeightPixels", NULL, 1080},
   {DP "lowMemoryKillerBackgroundAppThreshold", NULL, 209715200},
   {DP "lowMemoryKillerForegroundAppThreshold", NULL, 104857600},
   {DP "appBuildVariant", "release", 0},
   {DP "cpu64Bit", NULL, 1},
   {DP "memoryClass", NULL, 512},
   {DP "largeMemoryClass", NULL, 1024},
   {DP "isLowRamDevice", NULL, 0},
   {DP "deviceSku", "PS5", 0},
   {DP "socModel", "AMD Oberon", 0},
#undef DP
#define DS "com/roblox/engine/jni/model/DeviceStaticParams."
   /* read once in JNI_OnLoad; osVersion gates Vulkan ("Android version is too
    * old to activate Vulkan" when it parses as 0) */
   {DS "osVersion", "33", 0}, /* the app stores Integer.toString(Build.VERSION.SDK_INT) */
   {DS "deviceName", "PlayStation 5", 0},
   {DS "manufacturer", "Sony", 0},
   {DS "deviceSku", "PS5", 0},
   {DS "socModel", "AMD Oberon", 0},
   {DS "appVersion", "2.746.1266", 0},
   {DS "appBuildVariant", "release", 0},
   {DS "cpu64Bit", NULL, 1},
#undef DS
   {"com/roblox/engine/jni/model/PlatformParams.assetFolderPath", "/app0/assets/content/", 0},
   {"com/roblox/engine/jni/model/PlatformParams.dpiScale", NULL, 1.0},
   {"com/roblox/engine/jni/model/PlatformParams.isTouchDevice", NULL, 0},
   {"android/util/DisplayMetrics.density", NULL, 1.0},
   {"android/util/DisplayMetrics.densityDpi", NULL, 160},
   {"android/util/DisplayMetrics.widthPixels", NULL, 1920},
   {"android/util/DisplayMetrics.heightPixels", NULL, 1080},
   {"android/util/DisplayMetrics.xdpi", NULL, 160},
   {"android/util/DisplayMetrics.ydpi", NULL, 160},
   {"android/content/res/Configuration.orientation", NULL, 2},
   {"android/content/res/Configuration.screenWidthDp", NULL, 960},
   {"android/content/res/Configuration.screenHeightDp", NULL, 540},
   {"android/content/res/Configuration.smallestScreenWidthDp", NULL, 540},
   {"android/content/res/Configuration.densityDpi", NULL, 160},
   {"android/content/res/Configuration.fontScale", NULL, 1.0},
   {"android/content/res/Configuration.screenLayout", NULL, 0x14},
   {"android/content/res/Configuration.uiMode", NULL, 0x24},
   {"android/content/res/Configuration.touchscreen", NULL, 1},
   {"android/content/res/Configuration.keyboard", NULL, 1},
   {"android/content/res/Configuration.navigation", NULL, 1},
   {"android/content/res/Configuration.mcc", NULL, 0},
   {"android/content/res/Configuration.mnc", NULL, 0},
   /* android.os.Build: consistent with DeviceParams (Android 13 = SDK 33) */
   {"android/os/Build$VERSION.SDK_INT", NULL, 33},
   {"android/os/Build$VERSION.RELEASE", "13", 0},
   {"android/os/Build$VERSION.CODENAME", "REL", 0},
   {"android/os/Build$VERSION.INCREMENTAL", "1", 0},
   {"android/os/Build.MODEL", "PlayStation 5", 0},
   {"android/os/Build.MANUFACTURER", "Sony", 0},
   {"android/os/Build.BRAND", "Sony", 0},
   {"android/os/Build.DEVICE", "ps5", 0},
   {"android/os/Build.PRODUCT", "ps5", 0},
   {"android/os/Build.HARDWARE", "ps5", 0},
   {"android/os/Build.BOARD", "oberon", 0},
   {"android/os/Build.FINGERPRINT", "Sony/ps5/ps5:13/TQ3A.230901.001/1:user/release-keys", 0},
   {"android/os/Build.TYPE", "user", 0},
   {"android/os/Build.TAGS", "release-keys", 0},
   {NULL, NULL, 0},
};

static const struct fj_field_answer *
fj_field(void *id, char *key, size_t keylen)
{
   const struct fj_obj *f = id;
   snprintf(key, keylen, "%s.%s", f && f->magic == FJ_MAGIC && f->cls ? fj_name(f->cls) : "?",
            f && f->magic == FJ_MAGIC ? f->text : "?");
   /* UI size on the TV: the app lays itself out for the window in dp
    * (pixels / scale). At 1.0 a 1080p window is a wide desktop and every
    * element is small from the sofa; ROBLOX_UI_SCALE (default 1 since the swapchain is the 1080p surface; it was 2 while Roblox drew at 4K) sets it;
    * the size of a large tablet held close. */
   if (strstr(key, "PlatformParams.dpiScale") || strstr(key, "DisplayMetrics.density") ||
       strstr(key, "DisplayMetrics.densityDpi") || strstr(key, "DisplayMetrics.xdpi") ||
       strstr(key, "DisplayMetrics.ydpi")) {
      static struct fj_field_answer scaled[5];
      static const char *const names[5] = {"PlatformParams.dpiScale", "DisplayMetrics.densityDpi",
                                           "DisplayMetrics.density", "DisplayMetrics.xdpi", "DisplayMetrics.ydpi"};
      const char *e = getenv("ROBLOX_UI_SCALE");
      double k = e ? atof(e) : 1.0;
      if (k < 0.5 || k > 4)
         k = 1.0;
      for (int i = 0; i < 5; i++)
         if (strstr(key, names[i])) {
            scaled[i].field = names[i];
            scaled[i].string = NULL;
            scaled[i].value = i == 0 || i == 2 ? k : 160.0 * k;
            return &scaled[i];
         }
   }
   for (const struct fj_field_answer *a = fj_field_answers; a->field; a++)
      if (strcmp(a->field, key) == 0)
         return a;
   return NULL;
}
static unsigned fj_field_reads;
static void
fj_field_say(const char *kind, const char *key, const struct fj_field_answer *a)
{
   if (fj_field_reads++ < 200)
      fj_say("jni: %s %s -> %s", kind, key, a ? "answered" : "zero");
}
static void *
fj_GetObjectField(fj_env *e, void *o, void *id)
{
   (void)o;
   char key[256];
   const struct fj_field_answer *a = fj_field(id, key, sizeof(key));
   fj_field_say("GetObjectField", key, a);
   return a && a->string ? fj_NewStringUTF(e, a->string) : NULL;
}
static long long
fj_GetLongField(fj_env *e, void *o, void *id)
{
   (void)e;
   (void)o;
   char key[256];
   const struct fj_field_answer *a = fj_field(id, key, sizeof(key));
   fj_field_say("GetField", key, a);
   return a ? (long long)a->value : 0;
}
static float
fj_GetFloatField(fj_env *e, void *o, void *id)
{
   (void)e;
   (void)o;
   char key[256];
   const struct fj_field_answer *a = fj_field(id, key, sizeof(key));
   fj_field_say("GetFloatField", key, a);
   return a ? (float)a->value : 0.0f;
}
static double
fj_GetDoubleField(fj_env *e, void *o, void *id)
{
   return (double)fj_GetFloatField(e, o, id);
}

/* ---- objects and arrays made by the library ------------------------------- */
static void *
fj_AllocObject(fj_env *e, void *cls)
{
   (void)e;
   struct fj_obj *o = fj_new(FJ_OBJECT, fj_name(cls), NULL);
   if (o && cls && ((struct fj_obj *)cls)->magic == FJ_MAGIC)
      o->cls = cls;
   return o;
}
static unsigned fj_new_objects;
static void *
fj_NewObject(fj_env *e, void *cls, void *ctor, ...)
{
   (void)ctor;
   if (fj_new_objects++ < 100)
      fj_say("jni: NewObject %s", fj_name(cls));
   return fj_AllocObject(e, cls);
}
static void *fj_NewObjectV(fj_env *e, void *cls, void *ctor, void *args) { (void)args; return fj_NewObject(e, cls, ctor); }
static unsigned char fj_IsInstanceOf(fj_env *e, void *o, void *cls) { (void)e; (void)cls; return o != NULL; }

static struct fj_obj *
fj_array(long n, int size, const char *what)
{
   struct fj_obj *a = fj_new(FJ_ARRAY, what, NULL);
   if (!a)
      return NULL;
   a->length = n < 0 ? 0 : n;
   a->elem_size = size;
   a->data = calloc((size_t)(a->length ? a->length : 1), (size_t)size);
   return a;
}
static int
fj_GetArrayLength(fj_env *e, void *arr)
{
   (void)e;
   const struct fj_obj *a = arr;
   return a && a->magic == FJ_MAGIC && a->kind == FJ_ARRAY ? (int)a->length : 0;
}
static void *fj_NewObjectArray(fj_env *e, int n, void *cls, void *init)
{
   (void)e;
   struct fj_obj *a = fj_array(n, 8, "Object[]");
   for (int i = 0; a && i < n; i++)
      ((void **)a->data)[i] = init;
   (void)cls;
   return a;
}
static void *
fj_GetObjectArrayElement(fj_env *e, void *arr, int i)
{
   (void)e;
   const struct fj_obj *a = arr;
   return a && a->kind == FJ_ARRAY && i >= 0 && i < a->length ? ((void **)a->data)[i] : NULL;
}
static void
fj_SetObjectArrayElement(fj_env *e, void *arr, int i, void *v)
{
   (void)e;
   struct fj_obj *a = arr;
   if (a && a->kind == FJ_ARRAY && i >= 0 && i < a->length)
      ((void **)a->data)[i] = v;
}
#define FJ_PRIM(Name, size)                                                                           \
   static void *fj_New##Name##Array(fj_env *e, int n) { (void)e; return fj_array(n, size, #Name "[]"); } \
   static void *fj_Get##Name##ArrayElements(fj_env *e, void *arr, unsigned char *copy)               \
   {                                                                                                 \
      (void)e;                                                                                       \
      if (copy)                                                                                      \
         *copy = 0;                                                                                  \
      return arr ? ((struct fj_obj *)arr)->data : NULL;                                              \
   }                                                                                                 \
   static void fj_Release##Name##ArrayElements(fj_env *e, void *arr, void *p, int mode)              \
   {                                                                                                 \
      (void)e, (void)arr, (void)p, (void)mode;                                                       \
   }                                                                                                 \
   static void fj_Get##Name##ArrayRegion(fj_env *e, void *arr, int at, int n, void *out)            \
   {                                                                                                 \
      (void)e;                                                                                       \
      struct fj_obj *a = arr;                                                                        \
      if (a && at >= 0 && n >= 0 && at + n <= a->length)                                             \
         memcpy(out, (char *)a->data + (size_t)at * size, (size_t)n * size);                         \
   }                                                                                                 \
   static void fj_Set##Name##ArrayRegion(fj_env *e, void *arr, int at, int n, const void *in)       \
   {                                                                                                 \
      (void)e;                                                                                       \
      struct fj_obj *a = arr;                                                                        \
      if (a && at >= 0 && n >= 0 && at + n <= a->length)                                             \
         memcpy((char *)a->data + (size_t)at * size, in, (size_t)n * size);                          \
   }
FJ_PRIM(Boolean, 1)
FJ_PRIM(Byte, 1)
FJ_PRIM(Char, 2)
FJ_PRIM(Short, 2)
FJ_PRIM(Int, 4)
FJ_PRIM(Long, 8)
FJ_PRIM(Float, 4)
FJ_PRIM(Double, 8)
static void fj_SetField(void) {}

/* ---- JavaVM -------------------------------------------------------------- */

/* Real JNI: GetEnv on a thread that never attached returns JNI_EDETACHED,
 * and the library then attaches it (setting up its own per-thread state). */
static pthread_key_t fj_attached_key;
static pthread_once_t fj_attached_once = PTHREAD_ONCE_INIT;
static void fj_attached_init(void) { pthread_key_create(&fj_attached_key, NULL); }
/* Each attached thread has its own JNIEnv, as on Android: the library
 * replaces env->functions with its own hooking table per thread, and keeps
 * the table it found as the "original" to forward to. A shared env would
 * hand a second thread the hooked table as its original (endless recursion). */
static void
fj_mark_attached(fj_env *env)
{
   pthread_once(&fj_attached_once, fj_attached_init);
   pthread_setspecific(fj_attached_key, env);
}
static fj_env *
fj_thread_env(void)
{
   pthread_once(&fj_attached_once, fj_attached_init);
   return (fj_env *)pthread_getspecific(fj_attached_key);
}
static int
fj_is_attached(void)
{
   return fj_thread_env() != NULL;
}

static int fj_DestroyJavaVM(fj_vm *vm) { (void)vm; return 0; }
static int
fj_AttachCurrentThread(fj_vm *vm, fj_env **env, void *args)
{
   (void)vm;
   (void)args;
   fj_env *e = fj_thread_env();
   bionic_fs_self();
   if (!e) {
      e = (fj_env *)calloc(1, sizeof(*e));
      *e = (fj_env){(const void *const *)fj_env_table};
      fj_mark_attached(e);
      fj_say("jni: AttachCurrentThread (thread %p) -> own env %p", (void *)pthread_self(), (void *)e);
   }
   *env = e;
   return 0;
}
static int
fj_DetachCurrentThread(fj_vm *vm)
{
   (void)vm;
   fj_mark_attached(NULL);
   return 0;
}

static int
fj_GetEnv(fj_vm *vm, fj_env **env, int version)
{
   (void)vm;
   if (!fj_is_attached()) {
      fj_say("jni: GetEnv(version 0x%x): thread %p not attached", (unsigned)version, (void *)pthread_self());
      *env = NULL;
      return -2; /* JNI_EDETACHED */
   }
   *env = fj_thread_env();
   return 0;
}

/* ---- building the tables -------------------------------------------------- */

struct fj_impl {
   const char *name;
   void *fn;
};

static const struct fj_impl fj_env_impls[] = {
   {"GetVersion", (void *)fj_GetVersion},
   {"FindClass", (void *)fj_FindClass},
   {"GetObjectClass", (void *)fj_GetObjectClass},
   {"GetMethodID", (void *)fj_GetMethodID},
   {"GetStaticMethodID", (void *)fj_GetStaticMethodID},
   {"GetFieldID", (void *)fj_GetFieldID},
   {"AllocObject", (void *)fj_AllocObject},
   {"NewObject", (void *)fj_NewObject},
   {"NewObjectV", (void *)fj_NewObjectV},
   {"NewObjectA", (void *)fj_NewObjectV},
   {"IsInstanceOf", (void *)fj_IsInstanceOf},
   {"GetArrayLength", (void *)fj_GetArrayLength},
   {"NewObjectArray", (void *)fj_NewObjectArray},
   {"GetObjectArrayElement", (void *)fj_GetObjectArrayElement},
   {"SetObjectArrayElement", (void *)fj_SetObjectArrayElement},
   {"NewBooleanArray", (void *)fj_NewBooleanArray},
   {"GetBooleanArrayElements", (void *)fj_GetBooleanArrayElements},
   {"ReleaseBooleanArrayElements", (void *)fj_ReleaseBooleanArrayElements},
   {"GetBooleanArrayRegion", (void *)fj_GetBooleanArrayRegion},
   {"SetBooleanArrayRegion", (void *)fj_SetBooleanArrayRegion},
   {"NewByteArray", (void *)fj_NewByteArray},
   {"GetByteArrayElements", (void *)fj_GetByteArrayElements},
   {"ReleaseByteArrayElements", (void *)fj_ReleaseByteArrayElements},
   {"GetByteArrayRegion", (void *)fj_GetByteArrayRegion},
   {"SetByteArrayRegion", (void *)fj_SetByteArrayRegion},
   {"NewCharArray", (void *)fj_NewCharArray},
   {"GetCharArrayElements", (void *)fj_GetCharArrayElements},
   {"ReleaseCharArrayElements", (void *)fj_ReleaseCharArrayElements},
   {"GetCharArrayRegion", (void *)fj_GetCharArrayRegion},
   {"SetCharArrayRegion", (void *)fj_SetCharArrayRegion},
   {"NewShortArray", (void *)fj_NewShortArray},
   {"GetShortArrayElements", (void *)fj_GetShortArrayElements},
   {"ReleaseShortArrayElements", (void *)fj_ReleaseShortArrayElements},
   {"GetShortArrayRegion", (void *)fj_GetShortArrayRegion},
   {"SetShortArrayRegion", (void *)fj_SetShortArrayRegion},
   {"NewIntArray", (void *)fj_NewIntArray},
   {"GetIntArrayElements", (void *)fj_GetIntArrayElements},
   {"ReleaseIntArrayElements", (void *)fj_ReleaseIntArrayElements},
   {"GetIntArrayRegion", (void *)fj_GetIntArrayRegion},
   {"SetIntArrayRegion", (void *)fj_SetIntArrayRegion},
   {"NewLongArray", (void *)fj_NewLongArray},
   {"GetLongArrayElements", (void *)fj_GetLongArrayElements},
   {"ReleaseLongArrayElements", (void *)fj_ReleaseLongArrayElements},
   {"GetLongArrayRegion", (void *)fj_GetLongArrayRegion},
   {"SetLongArrayRegion", (void *)fj_SetLongArrayRegion},
   {"NewFloatArray", (void *)fj_NewFloatArray},
   {"GetFloatArrayElements", (void *)fj_GetFloatArrayElements},
   {"ReleaseFloatArrayElements", (void *)fj_ReleaseFloatArrayElements},
   {"GetFloatArrayRegion", (void *)fj_GetFloatArrayRegion},
   {"SetFloatArrayRegion", (void *)fj_SetFloatArrayRegion},
   {"NewDoubleArray", (void *)fj_NewDoubleArray},
   {"GetDoubleArrayElements", (void *)fj_GetDoubleArrayElements},
   {"ReleaseDoubleArrayElements", (void *)fj_ReleaseDoubleArrayElements},
   {"GetDoubleArrayRegion", (void *)fj_GetDoubleArrayRegion},
   {"SetDoubleArrayRegion", (void *)fj_SetDoubleArrayRegion},
   {"SetObjectField", (void *)fj_SetField},
   {"SetStaticObjectField", (void *)fj_SetField},
   {"SetBooleanField", (void *)fj_SetField},
   {"SetStaticBooleanField", (void *)fj_SetField},
   {"SetByteField", (void *)fj_SetField},
   {"SetStaticByteField", (void *)fj_SetField},
   {"SetCharField", (void *)fj_SetField},
   {"SetStaticCharField", (void *)fj_SetField},
   {"SetShortField", (void *)fj_SetField},
   {"SetStaticShortField", (void *)fj_SetField},
   {"SetIntField", (void *)fj_SetField},
   {"SetStaticIntField", (void *)fj_SetField},
   {"SetLongField", (void *)fj_SetField},
   {"SetStaticLongField", (void *)fj_SetField},
   {"SetFloatField", (void *)fj_SetField},
   {"SetStaticFloatField", (void *)fj_SetField},
   {"SetDoubleField", (void *)fj_SetField},
   {"SetStaticDoubleField", (void *)fj_SetField},
   {"GetObjectField", (void *)fj_GetObjectField},
   {"GetBooleanField", (void *)fj_GetLongField},
   {"GetByteField", (void *)fj_GetLongField},
   {"GetCharField", (void *)fj_GetLongField},
   {"GetShortField", (void *)fj_GetLongField},
   {"GetIntField", (void *)fj_GetLongField},
   {"GetLongField", (void *)fj_GetLongField},
   {"GetFloatField", (void *)fj_GetFloatField},
   {"GetStaticObjectField", (void *)fj_GetObjectField},
   {"GetStaticBooleanField", (void *)fj_GetLongField},
   {"GetStaticByteField", (void *)fj_GetLongField},
   {"GetStaticCharField", (void *)fj_GetLongField},
   {"GetStaticShortField", (void *)fj_GetLongField},
   {"GetStaticIntField", (void *)fj_GetLongField},
   {"GetStaticLongField", (void *)fj_GetLongField},
   {"GetStaticFloatField", (void *)fj_GetFloatField},
   {"GetStaticDoubleField", (void *)fj_GetDoubleField},
   {"GetDoubleField", (void *)fj_GetDoubleField},
   {"GetStaticFieldID", (void *)fj_GetStaticFieldID},
   {"RegisterNatives", (void *)fj_RegisterNatives},
   {"UnregisterNatives", (void *)fj_UnregisterNatives},
   {"NewGlobalRef", (void *)fj_Ref},
   {"NewLocalRef", (void *)fj_Ref},
   {"NewWeakGlobalRef", (void *)fj_Ref},
   {"DeleteGlobalRef", (void *)fj_Unref},
   {"DeleteLocalRef", (void *)fj_Unref},
   {"DeleteWeakGlobalRef", (void *)fj_Unref},
   {"ExceptionCheck", (void *)fj_ExceptionCheck},
   {"ExceptionOccurred", (void *)fj_ExceptionOccurred},
   {"ExceptionClear", (void *)fj_ExceptionVoid},
   {"ExceptionDescribe", (void *)fj_ExceptionVoid},
   {"PushLocalFrame", (void *)fj_PushLocalFrame},
   {"PopLocalFrame", (void *)fj_PopLocalFrame},
   {"EnsureLocalCapacity", (void *)fj_EnsureLocalCapacity},
   {"IsSameObject", (void *)fj_IsSameObject},
   {"GetJavaVM", (void *)fj_GetJavaVM},
   {"NewStringUTF", (void *)fj_NewStringUTF},
   {"GetStringUTFChars", (void *)fj_GetStringUTFChars},
   {"ReleaseStringUTFChars", (void *)fj_ReleaseStringUTFChars},
   {"GetStringUTFLength", (void *)fj_GetStringUTFLength},
   {"GetStringLength", (void *)fj_GetStringLength},
   {"GetStringUTFRegion", (void *)fj_GetStringUTFRegion},
   {"GetObjectRefType", (void *)fj_GetObjectRefType},
   {NULL, NULL},
};

static const struct fj_impl fj_vm_impls[] = {
   {"DestroyJavaVM", (void *)fj_DestroyJavaVM},
   {"AttachCurrentThread", (void *)fj_AttachCurrentThread},
   {"AttachCurrentThreadAsDaemon", (void *)fj_AttachCurrentThread},
   {"DetachCurrentThread", (void *)fj_DetachCurrentThread},
   {"GetEnv", (void *)fj_GetEnv},
   {NULL, NULL},
};

static void *
fj_lookup(const struct fj_impl *impls, const char *name)
{
   for (const struct fj_impl *i = impls; i->name; i++)
      if (strcmp(i->name, name) == 0)
         return i->fn;
   return NULL;
}

/* Fills both tables; slots without an implementation get trap(name). Returns
 * the JavaVM pointer to hand to JNI_OnLoad. */
static void *
fj_create(void (*log)(const char *), void *(*trap)(const char *name, void *ctx))
{
   fj_log = log;
   fj_mark_attached(&fj_the_env); /* the creating thread is Android's main (UI) thread */
   static char names[FJ_ENV_SLOTS + FJ_VM_SLOTS][48];
   unsigned provided = 0, trapped = 0;
   for (size_t i = 0; i < FJ_ENV_SLOTS; i++) {
      void *fn = strncmp(jni_env_slot_names[i], "reserved", 8) ? fj_lookup(fj_env_impls, jni_env_slot_names[i]) : NULL;
      if (!fn && strncmp(jni_env_slot_names[i], "reserved", 8))
         fn = fj_impl_for_call(jni_env_slot_names[i]);
      if (!fn && strncmp(jni_env_slot_names[i], "reserved", 8)) {
         snprintf(names[i], sizeof(names[i]), "JNIEnv::%s", jni_env_slot_names[i]);
         fn = trap(names[i], NULL);
         trapped++;
      } else if (fn) {
         provided++;
      }
      fj_env_table[i] = fn;
   }
   for (size_t i = 0; i < FJ_VM_SLOTS; i++) {
      void *fn = strncmp(jni_vm_slot_names[i], "reserved", 8) ? fj_lookup(fj_vm_impls, jni_vm_slot_names[i]) : NULL;
      if (!fn && strncmp(jni_vm_slot_names[i], "reserved", 8)) {
         snprintf(names[FJ_ENV_SLOTS + i], sizeof(names[0]), "JavaVM::%s", jni_vm_slot_names[i]);
         fn = trap(names[FJ_ENV_SLOTS + i], NULL);
         trapped++;
      } else if (fn) {
         provided++;
      }
      fj_vm_table[i] = fn;
   }
   fj_say("jni: stand-in VM ready: %u functions provided, %u logged as not provided yet", provided, trapped);
   return &fj_the_vm;
}

static void *fj_env_ptr(void) { return &fj_the_env; }

#endif
