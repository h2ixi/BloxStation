#include <sys/ioctl.h>
#include <sys/filio.h>
/*
 * Bionic (Android libc) compatibility for libroblox.so on the PS5.
 *
 * libroblox.so was built against Android's Bionic. The PS5 title runs on
 * FreeBSD-derived system libraries, so each import is either
 *  - DIRECT: the same function with the same ABI (math, string, memory,
 *    stdio on opaque FILE pointers, ...), bound to the title's own copy, or
 *  - SHIMMED: a wrapper below that converts what differs (pthread object
 *    layouts and static initialisers, clock ids, mmap flags, sysconf names,
 *    the _chk fortify entry points, locale objects, data symbols), or
 *  - left to the caller's trap resolver, which logs the call by name.
 *
 * Included by radv_smoke.c after android_elf_loader.h, on the console only.
 */
#ifndef BIONIC_SHIMS_H
#define BIONIC_SHIMS_H

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ---- direct imports ------------------------------------------------------
 * Declared with assembler labels so no prototype here conflicts with the
 * system headers'; only the address is taken. */
#define BIONIC_DIRECT_LIST(X)                                                                      \
   X(acos) X(acosf) X(asin) X(asinf) X(atan) X(atan2) X(atan2f) X(atanf) X(cbrt) X(cbrtf)          \
   X(ceilf) X(cos) X(cosf) X(cosh) X(coshf) X(erfcf) X(erff) X(exp) X(exp2) X(exp2f) X(expf)        \
   X(expm1) X(floorf) X(fmod) X(fmodf) X(frexp) X(frexpf) X(hypot) X(hypotf) X(ilogb) X(ldexp)       \
   X(ldexpf) X(llround) X(llroundf) X(log) X(log10) X(log10f) X(log2) X(log2f) X(logf) X(lround)    \
   X(lroundf) X(modf) X(modff) X(nan) X(nextafterf) X(pow) X(powf) X(powl) X(remainderf)           \
   X(remquof) X(round) X(scalbn) X(sin) X(sincos) X(sincosf) X(sinf) X(sinh) X(sinhf) X(tan)        \
   X(tanf) X(tanh) X(tanhf) X(finitef) X(setjmp) X(longjmp)                                                           \
   X(memchr) X(memcmp) X(memcpy) X(memmove) X(memrchr) X(memset) X(strcasecmp) X(strcat)           \
   X(strchr) X(strcmp) X(strcpy) X(strcspn) X(strlen) X(strncasecmp) X(strncat) X(strncmp)         \
   X(strncpy) X(strnlen) X(strpbrk) X(strrchr) X(strspn) X(strstr)            \
   X(tolower) X(isspace) X(btowc) X(wctob)                                              \
   X(atof) X(atoi) X(atol) X(atoll) X(strtod) X(strtof) X(strtol) X(strtoll) X(strtoul)            \
   X(strtoull) X(bsearch) X(qsort) X(ldiv) X(rand) X(srand)                                        \
   X(malloc) X(calloc) X(realloc) X(free)                                                          \
   X(fgets) X(fputc) X(fputs) X(fread) X(fprintf)           \
   X(vfprintf) X(printf) X(puts) X(snprintf) X(vsnprintf) X(sscanf) X(vsscanf) X(fscanf)           \
   X(vasprintf) X(setvbuf) X(fseek) X(ftell) X(feof) X(ferror) X(clearerr) X(fileno) X(getc)       \
   X(ungetc) X(fdopen) X(fseeko) X(ftello)                                     \
   X(time) X(difftime) X(gmtime) X(gmtime_r) X(localtime) X(localtime_r) X(mktime) X(strftime)     \
   X(gettimeofday) X(nanosleep) X(getenv) X(strerror) X(sched_yield)                      \
   X(pthread_self) X(pthread_equal) X(pthread_detach) X(pthread_join)              \
   X(pthread_key_create) X(pthread_key_delete) X(pthread_getspecific) X(pthread_setspecific)       \
   X(getpid) X(getuid) X(geteuid)                            \
   X(isatty) X(usleep) X(fsync) X(ftruncate)

#define BIONIC_DECLARE_DIRECT(name) extern char bionic_direct_##name[] __asm__(#name);
BIONIC_DIRECT_LIST(BIONIC_DECLARE_DIRECT)
#undef BIONIC_DECLARE_DIRECT

/* ---- logging -------------------------------------------------------------- */

static void (*bionic_report)(const char *line);

static void
bionic_say(const char *fmt, ...)
{
   char line[400];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   /* Lines the running app repeats many times a second (CPU-speed polls,
    * cache stats, memory probes, unmaps): every report is a klog write and
    * a flushed file write, which cost the engine most of its frame rate.
    * Each kind is shown 20 times, then counted silently.
    * ROBLOX_VERBOSE=1 shows everything. */
   static int verbose = -1;
   if (verbose < 0)
      verbose = getenv("ROBLOX_VERBOSE") != NULL;
   if (!verbose) {
      static const char *const noisy[] = {"cpufreq", "oom_score", "rbx-storage", "memProfStorage", "munmap(",
                                          "mmap(", "LocalStorage", "dmem:", "Failed to mmap file", "cpu/",
                                          "AAssetManager_open", "stat(/data/roblox/cache", "time_in_state", "file> ", "wob/wob-", NULL};
      static unsigned seen[16];
      for (int i = 0; noisy[i]; i++)
         if (strstr(line, noisy[i])) {
            if (__atomic_add_fetch(&seen[i], 1, __ATOMIC_RELAXED) > 20)
               return;
            break;
         }
   }
   if (bionic_report)
      bionic_report(line);
}

/* Counts the engine's "app ready", "game loaded" and "left the game" log
 * lines: the controller is announced again after each (roblox_pad_thread). */
static unsigned bionic_app_events;
/* Counts sign-ins and account switches ("DID_LOG_IN", "DID_SWITCH_ACCOUNT"):
 * the title then saves the engine's current login (roblox_cookies.h). */
static unsigned bionic_login_events;
static unsigned bionic_logout_events; /* DID_LOG_OUT */
/* The signed-in player's numeric user id ("userId" in DID_LOG_IN /
 * DID_SWITCH_ACCOUNT), 0 when signed out: per-player first-run things. */
static long long bionic_user_id;
/* In a game (not the app's own menus): "onGameLoaded: placeId:N" with N != 0
 * until "leaveUGCGameInternal". */
static int bionic_in_game;
static long long bionic_place_id; /* the game's place id, 0 in the menus */
/* The menus' page last reported ready ("type(APP_READY, 10), data(Home)"). */
static char bionic_app_page[48];
/* Counts render views the engine dropped ("::pause destroyView:true", a game
 * started from the menus after an earlier game) and made again ("reset the
 * view"); Android then hands the surface over again (roblox_pad_thread). */
static unsigned bionic_view_destroyed;

static int
bionic_log_write(int prio, const char *tag, const char *text)
{
   if (text && strstr(text, "type(DID_LOG_OUT"))
      __atomic_add_fetch(&bionic_logout_events, 1, __ATOMIC_RELAXED);
   if (text && strstr(text, "type(DID_LOG_OUT"))
      __atomic_store_n(&bionic_user_id, 0, __ATOMIC_RELAXED);
   if (text && (strstr(text, "type(DID_LOG_IN") || strstr(text, "type(DID_SWITCH_ACCOUNT"))) {
      __atomic_add_fetch(&bionic_login_events, 1, __ATOMIC_RELAXED);
      const char *u = strstr(text, "\"userId\":");
      if (u) {
         long long id = strtoll(u + 9, NULL, 10);
         if (id > 0)
            __atomic_store_n(&bionic_user_id, id, __ATOMIC_RELAXED);
      }
   }
   if (text && (strstr(text, "type(APP_READY") || strstr(text, "onGameLoaded:")))
      __atomic_add_fetch(&bionic_app_events, 1, __ATOMIC_RELAXED);
   if (text) {
      const char *r = strstr(text, "type(APP_READY");
      const char *d = r ? strstr(r, "data(") : NULL;
      if (d) {
         char page[sizeof(bionic_app_page)];
         size_t n = 0;
         for (d += 5; *d && *d != ')' && n < sizeof(page) - 1; d++)
            page[n++] = *d;
         page[n] = 0;
         /* RootSwitchNavigator follows every page; it is not a page itself */
         if (strcmp(page, "RootSwitchNavigator") != 0)
            memcpy(bionic_app_page, page, n + 1);
      }
      static int view_dropped;
      if (strstr(text, "::pause destroyView:true"))
         view_dropped = 1;
      else if (view_dropped && strstr(text, "replaceDataModel: reset the view")) {
         /* the new render view exists now */
         view_dropped = 0;
         __atomic_add_fetch(&bionic_view_destroyed, 1, __ATOMIC_RELEASE);
      }
      const char *g = strstr(text, "onGameLoaded: placeId:");
      if (g) {
         __atomic_store_n(&bionic_place_id, atoll(g + 22), __ATOMIC_RELAXED);
         __atomic_store_n(&bionic_in_game, g[22] != '0', __ATOMIC_RELAXED);
      } else if (strstr(text, "leaveUGCGameInternal")) {
         __atomic_store_n(&bionic_place_id, 0, __ATOMIC_RELAXED);
         __atomic_store_n(&bionic_in_game, 0, __ATOMIC_RELAXED);
         /* back in the app's menus: they need the controller announced again
          * (without it no button reached them after leaving a game) */
         __atomic_add_fetch(&bionic_app_events, 1, __ATOMIC_RELAXED);
      }
   }
   bionic_say("[liblog %d %s] %s", prio, tag ? tag : "", text ? text : "");
   return 1;
}

static int
bionic_log_buf_write(int buf, int prio, const char *tag, const char *text)
{
   (void)buf;
   return bionic_log_write(prio, tag, text);
}

static int
bionic_log_print(int prio, const char *tag, const char *fmt, ...)
{
   char text[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);
   return bionic_log_write(prio, tag, text);
}

static void
bionic_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
   char text[512] = "";
   if (fmt) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(text, sizeof(text), fmt, ap);
      va_end(ap);
   }
   bionic_say("[liblog ASSERT %s] %s %s", tag ? tag : "", cond ? cond : "", text);
   abort();
}

static void
bionic_set_abort_message(const char *msg)
{
   bionic_say("android_set_abort_message: %s", msg ? msg : "");
}

static void
bionic_assert_fail(const char *file, int line, const char *expr)
{
   bionic_say("__assert %s:%d %s", file ? file : "?", line, expr ? expr : "?");
   abort();
}

static void
bionic_stack_chk_fail(void)
{
   bionic_say("__stack_chk_fail: stack smashing detected");
   abort();
}

/* ---- errno and data symbols ----------------------------------------------- */

static int *
bionic_errno(void)
{
   return &errno;
}

static uintptr_t bionic_stack_chk_guard = 0x2f8d3b1c5a6e7f90ull;
static FILE *bionic_stdin, *bionic_stdout, *bionic_stderr;
static uint8_t bionic_sF[3 * 152];
/* The engine's environment. Its allocator (mimalloc) reads options from
 * here: by default it reserves 1 GiB arenas, which the direct-memory layer
 * refuses (>= 256 MiB), so it fell back to mapping and unmapping small
 * pieces thousands of times a second (zero-filled each time: frame
 * hitches). 64 MiB arenas fit. ROBLOX_MI_ARENA_KIB overrides, 0 leaves the
 * environment empty. */
static char bionic_environ_arena[64] = "MIMALLOC_ARENA_RESERVE=64MiB";
static char *bionic_environ_empty[2] = {bionic_environ_arena, NULL};
static char **bionic_environ = bionic_environ_empty;
static void
bionic_environ_setup(void)
{
   const char *e = getenv("ROBLOX_MI_ARENA_KIB");
   if (e && atoi(e) <= 0)
      bionic_environ_empty[0] = NULL;
   else if (e)
      snprintf(bionic_environ_arena, sizeof(bionic_environ_arena), "MIMALLOC_ARENA_RESERVE=%dKiB", atoi(e));
}
static char *bionic_optarg;
static int bionic_optind = 1;
static int bionic_daylight;
static long bionic_timezone;
static char *bionic_tzname[2] = {(char *)"UTC", (char *)"UTC"};
static const uint8_t bionic_in6addr_any[16];
static const uint8_t bionic_in6addr_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

/* ---- C++ runtime hooks ------------------------------------------------------ */

static int
bionic_cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
   (void)fn;
   (void)arg;
   (void)dso;
   return 0; /* the title never exits normally: nothing to run at exit */
}

static void
bionic_cxa_finalize(void *dso)
{
   (void)dso;
}

/* C++ thread_local destructors. They must run when a thread finishes: the
 * engine's allocator retires the thread's heap in one of them. Without that,
 * the next thread - which the system gives the same thread-pointer (%fs:0)
 * address - is taken for the dead thread's owner, and frees its spans on the
 * owner-only (non-atomic) path. Kept per thread in a key; run newest first,
 * including ones registered while running. */
struct bionic_tdtor {
   void (*fn)(void *);
   void *obj;
   struct bionic_tdtor *next;
};
static pthread_key_t bionic_tdtor_key;
static pthread_once_t bionic_tdtor_once = PTHREAD_ONCE_INIT;
static unsigned bionic_tdtor_registered, bionic_tdtor_ran;

static void
bionic_tdtor_run(void *head)
{
   struct bionic_tdtor *d = head;
   for (;;) {
      if (!d) {
         d = pthread_getspecific(bionic_tdtor_key);
         if (!d)
            break;
      }
      pthread_setspecific(bionic_tdtor_key, d->next);
      void (*fn)(void *) = d->fn;
      void *obj = d->obj;
      free(d);
      __atomic_add_fetch(&bionic_tdtor_ran, 1, __ATOMIC_RELAXED);
      fn(obj);
      d = NULL;
   }
   pthread_setspecific(bionic_tdtor_key, NULL);
}

static void bionic_tdtor_init(void) { pthread_key_create(&bionic_tdtor_key, bionic_tdtor_run); }

static int
bionic_cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso)
{
   (void)dso;
   pthread_once(&bionic_tdtor_once, bionic_tdtor_init);
   struct bionic_tdtor *d = malloc(sizeof(*d));
   if (!d)
      return -1;
   d->fn = fn;
   d->obj = obj;
   d->next = pthread_getspecific(bionic_tdtor_key);
   pthread_setspecific(bionic_tdtor_key, d);
   __atomic_add_fetch(&bionic_tdtor_registered, 1, __ATOMIC_RELAXED);
   return 0;
}

static int
bionic_register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *dso)
{
   (void)a;
   (void)b;
   (void)c;
   (void)dso;
   return 0;
}

/* ---- pthread ----------------------------------------------------------------
 * Bionic's objects are larger structs (mutex 40 bytes, cond 48, rwlock 56)
 * whose static initialisers are all zero; the system's are one pointer whose
 * NULL means "initialise on first use". So the first eight bytes of a Bionic
 * object hold the system object. Bionic's recursive/error-checking static
 * initialisers put a small type value there instead; those are turned into
 * real mutexes on first use. Attribute objects are Bionic's own (a long), and
 * converted when an object is initialised. */

enum { BIONIC_MUTEX_NORMAL = 0, BIONIC_MUTEX_RECURSIVE = 1, BIONIC_MUTEX_ERRORCHECK = 2 };

static unsigned bionic_threads;
static pthread_t bionic_thread_list[256];
static unsigned bionic_thread_list_n;

static pthread_mutex_t bionic_init_lock = PTHREAD_MUTEX_INITIALIZER;

static int
bionic_mutexattr_init(long *attr)
{
   *attr = 0;
   return 0;
}

static int
bionic_mutexattr_destroy(long *attr)
{
   (void)attr;
   return 0;
}

static int
bionic_mutexattr_settype(long *attr, int type)
{
   if (type < 0 || type > 2)
      return EINVAL;
   *attr = (*attr & ~3L) | type;
   return 0;
}

/* Linux clock ids, as Bionic passes them, to the system's. */
static clockid_t
bionic_clock(int linux_id)
{
   switch (linux_id) {
   case 0: return CLOCK_REALTIME;
   case 1: /* MONOTONIC */
   case 4: /* MONOTONIC_RAW */
   case 6: /* MONOTONIC_COARSE */
   case 7: /* BOOTTIME */ return CLOCK_MONOTONIC;
   case 5: return CLOCK_REALTIME; /* REALTIME_COARSE */
#ifdef CLOCK_PROCESS_CPUTIME_ID
   case 2: return CLOCK_PROCESS_CPUTIME_ID;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
   case 3: return CLOCK_THREAD_CPUTIME_ID;
#endif
   default: return CLOCK_MONOTONIC;
   }
}

static int
bionic_clock_gettime(int linux_id, struct timespec *ts)
{
   return clock_gettime(bionic_clock(linux_id), ts);
}

static long
bionic_clock_fn(void)
{
   /* Bionic's CLOCKS_PER_SEC is 1000000. */
   struct timespec ts;
#ifdef CLOCK_PROCESS_CPUTIME_ID
   clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
#else
   clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
   return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

/* Bionic's pthread_mutex_t is one 4-byte word, pthread_cond_t is 4 bytes and
 * pthread_rwlock_t is 32; the PS5's are far larger, and initialising one in
 * place would clobber the fields around it in the engine's structs. Every
 * lock therefore keeps its real object in a side table keyed by the address
 * of the Bionic one; the Bionic storage is never touched. */
enum { BSYNC_MUTEX = 1, BSYNC_COND = 2, BSYNC_RWLOCK = 3, BSYNC_SEM = 4 };
#define BIONIC_SYNC_BUCKETS 512
struct bionic_sync_entry {
   const void *key;
   int kind;
   void *real;
   struct bionic_sync_entry *next;
};
static struct bionic_sync_entry *bionic_sync_table[BIONIC_SYNC_BUCKETS];
static pthread_mutex_t bionic_sync_lock = PTHREAD_MUTEX_INITIALIZER;
/* Evidence counters, printed when the library faults: a lock or unlock that
 * arrives for an address the side table does not know, and a side-table entry
 * destroyed while another thread still held its lock. Both mean the critical
 * section stopped being exclusive. */
static unsigned bionic_sync_locked_unknown, bionic_sync_unlocked_unknown, bionic_sync_dropped_locked;

static void *bionic_sync_get(void *key, int kind, int (*make)(void **out, void *ctx), void *ctx);
static void bionic_sync_drop(void *key, int kind);

/* pthread_mutex_init etc. on an address whose earlier lock is still in the
 * table (the engine freed one object and a new one landed there): the stored
 * real object is destroyed and replaced, so a fresh lock is used. */
static void *
bionic_sync_replace(void *key, int kind, int (*make)(void **out, void *ctx), void *ctx)
{
   /* Build the new object first, then swap it in under the table lock: no
    * other thread ever sees an entry without a real object. The previous
    * object is deliberately leaked - a thread may still be blocked on it, and
    * destroying it under that thread would be worse than a few bytes. */
   void *fresh = NULL;
   if (make(&fresh, ctx) != 0 || !fresh)
      return NULL;
   const unsigned b = (unsigned)(((uintptr_t)key >> 3) * 2654435761u) % BIONIC_SYNC_BUCKETS;
   pthread_mutex_lock(&bionic_sync_lock);
   for (struct bionic_sync_entry *e = bionic_sync_table[b]; e; e = e->next)
      if (e->key == key && e->kind == kind) {
         e->real = fresh;
         pthread_mutex_unlock(&bionic_sync_lock);
         return fresh;
      }
   struct bionic_sync_entry *e = malloc(sizeof(*e));
   if (!e) {
      pthread_mutex_unlock(&bionic_sync_lock);
      return NULL;
   }
   e->key = key;
   e->kind = kind;
   e->real = fresh;
   e->next = bionic_sync_table[b];
   bionic_sync_table[b] = e;
   pthread_mutex_unlock(&bionic_sync_lock);
   return fresh;
}

static void
bionic_sync_drop(void *key, int kind)
{
   /* Unlink only. The real object is never destroyed or freed: another thread
    * may have looked it up a moment ago and still be inside lock/wait on it,
    * and freeing it under that thread is a use-after-free. A held mutex at
    * destroy time is still counted for the report. */
   const unsigned b = (unsigned)(((uintptr_t)key >> 3) * 2654435761u) % BIONIC_SYNC_BUCKETS;
   pthread_mutex_lock(&bionic_sync_lock);
   for (struct bionic_sync_entry **link = &bionic_sync_table[b]; *link; link = &(*link)->next) {
      struct bionic_sync_entry *e = *link;
      if (e->key == key && e->kind == kind) {
         *link = e->next;
         if (kind == BSYNC_MUTEX && e->real) {
            if (pthread_mutex_trylock((pthread_mutex_t *)e->real) == EBUSY)
               bionic_sync_dropped_locked++;
            else
               pthread_mutex_unlock((pthread_mutex_t *)e->real);
         }
         free(e);
         break;
      }
   }
   pthread_mutex_unlock(&bionic_sync_lock);
}

static void *
bionic_sync_get(void *key, int kind, int (*make)(void **out, void *ctx), void *ctx)
{
   const unsigned b = (unsigned)(((uintptr_t)key >> 3) * 2654435761u) % BIONIC_SYNC_BUCKETS;
   pthread_mutex_lock(&bionic_sync_lock);
   for (struct bionic_sync_entry *e = bionic_sync_table[b]; e; e = e->next)
      if (e->key == key && e->kind == kind) {
         void *r = e->real;
         pthread_mutex_unlock(&bionic_sync_lock);
         return r;
      }
   struct bionic_sync_entry *e = malloc(sizeof(*e));
   void *r = NULL;
   if (e && make(&r, ctx) == 0) {
      e->key = key;
      e->kind = kind;
      e->real = r;
      e->next = bionic_sync_table[b];
      bionic_sync_table[b] = e;
   } else {
      free(e);
      r = NULL;
   }
   pthread_mutex_unlock(&bionic_sync_lock);
   return r;
}

static int
bionic_mutex_type_from_word(void *m)
{
   const uintptr_t v = *(uintptr_t *)m;
   if (v != 0 && v < 0x10000)
      return (int)((v >> 14) & 3); /* Bionic: bits 14-15: 0 normal, 1 recursive, 2 error-check */
   return 0;
}

static int bionic_mutex_make(void **out, void *ctx)
{
   pthread_mutexattr_t a;
   pthread_mutexattr_init(&a);
   const int type = (int)(uintptr_t)ctx;
   pthread_mutexattr_settype(&a, type == BIONIC_MUTEX_RECURSIVE    ? PTHREAD_MUTEX_RECURSIVE
                                 : type == BIONIC_MUTEX_ERRORCHECK ? PTHREAD_MUTEX_ERRORCHECK
                                                                   : PTHREAD_MUTEX_NORMAL);
   pthread_mutex_t *m = malloc(sizeof(*m));
   if (!m)
      return ENOMEM;
   const int rc = pthread_mutex_init(m, &a);
   pthread_mutexattr_destroy(&a);
   *out = m;
   return rc;
}

/* A lock or unlock that arrives for an address the side table does not know is
 * a lost lock: bionic_sync_get then makes a fresh, unlocked real object, so the
 * critical section is no longer exclusive (counters are declared with the table
 * above). */
static int
bionic_sync_have(void *key, int kind)
{
   const unsigned b = (unsigned)(((uintptr_t)key >> 3) * 2654435761u) % BIONIC_SYNC_BUCKETS;
   int found = 0;
   pthread_mutex_lock(&bionic_sync_lock);
   for (struct bionic_sync_entry *e = bionic_sync_table[b]; e; e = e->next)
      if (e->key == key && e->kind == kind) {
         found = 1;
         break;
      }
   pthread_mutex_unlock(&bionic_sync_lock);
   return found;
}

static pthread_mutex_t *
bionic_mutex(void *m)
{
   return (pthread_mutex_t *)bionic_sync_get(m, BSYNC_MUTEX, bionic_mutex_make,
                                             (void *)(uintptr_t)bionic_mutex_type_from_word(m));
}
static int
bionic_mutex_lock(void *m)
{
   if (!bionic_sync_have(m, BSYNC_MUTEX))
      bionic_sync_locked_unknown++;
   pthread_mutex_t *r = bionic_mutex(m);
   return r ? pthread_mutex_lock(r) : EINVAL;
}
static int
bionic_mutex_unlock(void *m)
{
   if (!bionic_sync_have(m, BSYNC_MUTEX))
      bionic_sync_unlocked_unknown++;
   pthread_mutex_t *r = bionic_mutex(m);
   return r ? pthread_mutex_unlock(r) : EINVAL;
}
static int bionic_mutex_trylock(void *m) { return bionic_mutex(m) ? pthread_mutex_trylock(bionic_mutex(m)) : EINVAL; }
static int bionic_mutex_init(void *m, const long *attr)
{
   const int type = attr ? (int)(*attr & 3) : bionic_mutex_type_from_word(m);
   return bionic_sync_replace(m, BSYNC_MUTEX, bionic_mutex_make, (void *)(uintptr_t)type) ? 0 : ENOMEM;
}
static int
bionic_mutex_destroy(void *m)
{
   bionic_sync_drop(m, BSYNC_MUTEX);
   return 0;
}

static int
bionic_cond_make(void **out, void *ctx)
{
   pthread_condattr_t a;
   pthread_condattr_init(&a);
   const int linux_clock = (int)(uintptr_t)ctx;
   if (linux_clock > 0)
      pthread_condattr_setclock(&a, bionic_clock(linux_clock));
   pthread_cond_t *c = malloc(sizeof(*c));
   if (!c)
      return ENOMEM;
   const int rc = pthread_cond_init(c, &a);
   pthread_condattr_destroy(&a);
   *out = c;
   return rc;
}
/* Bionic's pthread_condattr_t is a long: bit 0 pshared, clock id above it. */
static int bionic_condattr_init(long *a) { *a = 0; return 0; }
static int bionic_condattr_destroy(long *a) { (void)a; return 0; }
static int
bionic_condattr_setclock(long *a, int linux_clock)
{
   *a = (*a & 1) | ((long)linux_clock << 1);
   return 0;
}
static pthread_cond_t *
bionic_cond(void *c, const long *attr)
{
   const int linux_clock = attr && (*attr >> 1) ? (int)(*attr >> 1) : 0; /* Bionic default: realtime */
   return (pthread_cond_t *)bionic_sync_get(c, BSYNC_COND, bionic_cond_make, (void *)(uintptr_t)linux_clock);
}
static int bionic_cond_init(void *c, const long *attr)
{
   const int linux_clock = attr && (*attr >> 1) ? (int)(*attr >> 1) : 0;
   return bionic_sync_replace(c, BSYNC_COND, bionic_cond_make, (void *)(uintptr_t)linux_clock) ? 0 : ENOMEM;
}
static int bionic_cond_destroy(void *c) { bionic_sync_drop(c, BSYNC_COND); return 0; }
static int bionic_cond_signal(void *c) { pthread_cond_t *r = bionic_cond(c, NULL); return r ? pthread_cond_signal(r) : EINVAL; }
static int bionic_cond_broadcast(void *c) { pthread_cond_t *r = bionic_cond(c, NULL); return r ? pthread_cond_broadcast(r) : EINVAL; }
static int bionic_cond_wait(void *c, void *m)
{
   pthread_cond_t *cv = bionic_cond(c, NULL);
   return cv ? pthread_cond_wait(cv, bionic_mutex(m)) : EINVAL;
}
static int
bionic_cond_timedwait(void *c, void *m, const struct timespec *abstime)
{
   pthread_cond_t *cv = bionic_cond(c, NULL);
   const int rc = cv ? pthread_cond_timedwait(cv, bionic_mutex(m), abstime) : EINVAL;
   /* libc++ compares with Linux's ETIMEDOUT (110); the PS5's is 60. */
   return rc == ETIMEDOUT ? 110 : rc;
}

static int
bionic_rwlock_make(void **out, void *ctx)
{
   (void)ctx;
   pthread_rwlock_t *l = malloc(sizeof(*l));
   if (!l)
      return ENOMEM;
   const int rc = pthread_rwlock_init(l, NULL);
   *out = l;
   return rc;
}
static pthread_rwlock_t *bionic_rwlock(void *l) { return (pthread_rwlock_t *)bionic_sync_get(l, BSYNC_RWLOCK, bionic_rwlock_make, NULL); }
static int bionic_rwlock_init(void *l, const void *attr)
{
   (void)attr;
   return bionic_sync_replace(l, BSYNC_RWLOCK, bionic_rwlock_make, NULL) ? 0 : ENOMEM;
}
static int bionic_rwlock_destroy(void *l) { bionic_sync_drop(l, BSYNC_RWLOCK); return 0; }
static int bionic_rwlock_rdlock(void *l) { pthread_rwlock_t *r = bionic_rwlock(l); return r ? pthread_rwlock_rdlock(r) : EINVAL; }
static int bionic_rwlock_wrlock(void *l) { pthread_rwlock_t *r = bionic_rwlock(l); return r ? pthread_rwlock_wrlock(r) : EINVAL; }
static int bionic_rwlock_unlock(void *l) { pthread_rwlock_t *r = bionic_rwlock(l); return r ? pthread_rwlock_unlock(r) : EINVAL; }

/* Bionic's sem_t is 4 bytes; the PS5's is larger, so a sem_init in place
 * would clobber neighbouring fields: side table as well. */
static int
bionic_sem_make(void **out, void *ctx)
{
   sem_t *v = malloc(sizeof(*v));
   if (!v)
      return ENOMEM;
   const int rc = sem_init(v, 0, (unsigned)(uintptr_t)ctx);
   if (rc != 0) {
      free(v);
      return errno;
   }
   *out = v;
   return 0;
}
static sem_t *
bionic_sem(void *s, unsigned value)
{
   return (sem_t *)bionic_sync_get(s, BSYNC_SEM, bionic_sem_make, (void *)(uintptr_t)value);
}
static int bionic_sem_init(void *s, int pshared, unsigned value)
{
   (void)pshared;
   return bionic_sync_replace(s, BSYNC_SEM, bionic_sem_make, (void *)(uintptr_t)value) ? 0 : -1;
}
static int bionic_sem_destroy(void *s) { bionic_sync_drop(s, BSYNC_SEM); return 0; }
static int bionic_sem_wait(void *s) { sem_t *v = bionic_sem(s, 0); return v ? sem_wait(v) : -1; }
static int bionic_sem_trywait(void *s) { sem_t *v = bionic_sem(s, 0); return v ? sem_trywait(v) : -1; }
static int bionic_sem_post(void *s) { sem_t *v = bionic_sem(s, 0); return v ? sem_post(v) : -1; }
static int bionic_sem_getvalue(void *s, int *out) { sem_t *v = bionic_sem(s, 0); return v ? sem_getvalue(v, out) : -1; }

/* Timestamps for the engine's log lines, and the scheduler calls its thread
 * pool makes (it asks for real-time priorities; there is nothing to grant). */
static size_t bionic_strftime_l(char *out, size_t max, const char *fmt, const struct tm *tm, void *loc)
{
   (void)loc;
   return strftime(out, max, fmt, tm);
}
static int bionic_sched_get_priority_max(int policy) { (void)policy; return 99; }
static int bionic_sched_get_priority_min(int policy) { (void)policy; return 1; }
static int bionic_sched_setscheduler(int pid, int policy, const void *param)
{
   (void)pid;
   (void)policy;
   (void)param;
   return 0;
}
static int bionic_sched_getscheduler(int pid) { (void)pid; return 0; }

/* Bionic's pthread_once_t is an int: 0 never run, 1 running, 2 done. */
static int
bionic_once(int *once, void (*fn)(void))
{
   int state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
   while (state != 2) {
      int expected = 0;
      if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
         fn();
         __atomic_store_n(once, 2, __ATOMIC_RELEASE);
         return 0;
      }
      sched_yield();
      state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
   }
   return 0;
}

/* Bionic's pthread_attr_t: flags, stack base, stack size, guard size,
 * policy, priority. */
struct bionic_attr {
   uint32_t flags;
   void *stack_base;
   size_t stack_size;
   size_t guard_size;
   int32_t sched_policy;
   int32_t sched_priority;
};
_Static_assert(sizeof(struct bionic_attr) <= 56, "Bionic's pthread_attr_t is 56 bytes on x86_64");

static int
bionic_attr_init(struct bionic_attr *a)
{
   memset(a, 0, sizeof(*a));
   a->stack_size = 1024 * 1024;
   a->guard_size = 4096;
   return 0;
}
static int bionic_attr_destroy(struct bionic_attr *a) { (void)a; return 0; }
static int bionic_attr_setstacksize(struct bionic_attr *a, size_t s) { a->stack_size = s; return 0; }
static int bionic_attr_setdetachstate(struct bionic_attr *a, int d) { a->flags = (a->flags & ~1u) | (d ? 1u : 0u); return 0; }
static int bionic_attr_setschedparam(struct bionic_attr *a, const int *p) { a->sched_priority = p ? *p : 0; return 0; }
static int
bionic_attr_getstack(const struct bionic_attr *a, void **base, size_t *size)
{
   *base = a->stack_base;
   *size = a->stack_size;
   return 0;
}

int pthread_attr_get_np(pthread_t thread, pthread_attr_t *attr);

static int
bionic_getattr_np(pthread_t thread, struct bionic_attr *out)
{
   bionic_attr_init(out);
   pthread_attr_t a;
   pthread_attr_init(&a);
   int rc = pthread_attr_get_np(thread, &a);
   if (rc == 0) {
      void *base = NULL;
      size_t size = 0;
      if (pthread_attr_getstack(&a, &base, &size) == 0) {
         out->stack_base = base;
         out->stack_size = size;
      }
   }
   pthread_attr_destroy(&a);
   return rc;
}

#include <signal.h>

/* ---- signals ---------------------------------------------------------------
 * Android LP64 struct sigaction: int sa_flags @0, handler @8, sigset_t (8 bytes)
 * @16, sa_restorer @24 (bionic libc/include/bits/signal_types.h). Signal numbers
 * are Linux's (SIGBUS 7, SIGUSR1 10, ...), not the PS5's. A handler the engine
 * installs could not read a PS5 signal frame anyway, so its registrations are
 * virtual: recorded per Linux signal number and handed back as the "old" action,
 * never installed. The title's own fault reporter stays in charge. The one real
 * effect honoured is SIGPIPE ignore (13 on both), which sockets will need. */
struct bionic_sigaction { int flags; int pad; void *handler; uint64_t mask; void *restorer; };
_Static_assert(sizeof(struct bionic_sigaction) == 32, "Android LP64 struct sigaction is 32 bytes");
static struct bionic_sigaction bionic_sig_table[65];
static pthread_mutex_t bionic_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned bionic_sig_said;

static void bionic_sigemptyset(void *set) { memset(set, 0, 8); }
static void bionic_sigfillset(void *set) { memset(set, 0xff, 8); }
static int
bionic_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old)
{
   if (sig < 1 || sig > 64) {
      errno = EINVAL;
      return -1;
   }
   pthread_mutex_lock(&bionic_sig_lock);
   if (old)
      *old = bionic_sig_table[sig];
   if (act) {
      bionic_sig_table[sig] = *act;
      if (bionic_sig_said++ < 24)
         bionic_say("sigaction(linux %d): handler %p flags 0x%x recorded, not installed", sig, act->handler,
                    act->flags);
   }
   pthread_mutex_unlock(&bionic_sig_lock);
   if (act && sig == 13 && act->handler == (void *)1 /* SIG_IGN */)
      signal(SIGPIPE, SIG_IGN);
   return 0;
}
static int
bionic_pthread_sigmask(int how, const void *set, void *old)
{
   /* Blocking is virtual too (Linux and PS5 bit numbering differ); report an
    * empty previous mask. */
   (void)how;
   (void)set;
   if (old)
      memset(old, 0, 8);
   return 0;
}
static int
bionic_sigaltstack(const void *in, void *out)
{
   /* Linux stack_t {sp @0, int flags @8, size @16}; PS5 {sp, size, flags}.
    * SS_DISABLE is 2 on Linux, 4 on the PS5. */
   stack_t ni, oi;
   memset(&ni, 0, sizeof(ni));
   if (in) {
      int flags;
      memcpy(&ni.ss_sp, in, 8);
      memcpy(&flags, (const char *)in + 8, 4);
      memcpy(&ni.ss_size, (const char *)in + 16, 8);
      ni.ss_flags = (flags & 2) ? SS_DISABLE : 0;
   }
   memset(&oi, 0, sizeof(oi));
   const int rc = sigaltstack(in ? &ni : NULL, out ? &oi : NULL);
   if (rc == 0 && out) {
      int flags = ((oi.ss_flags & SS_DISABLE) ? 2 : 0) | ((oi.ss_flags & SS_ONSTACK) ? 1 : 0);
      memset(out, 0, 24);
      memcpy(out, &oi.ss_sp, 8);
      memcpy((char *)out + 8, &flags, 4);
      memcpy((char *)out + 16, &oi.ss_size, 8);
   }
   return rc;
}
static void *
bionic_signal(int sig, void *handler)
{
   struct bionic_sigaction act = {0}, old = {0};
   act.handler = handler;
   if (bionic_sigaction(sig, &act, &old) != 0)
      return (void *)-1; /* SIG_ERR */
   return old.handler;
}

/* Every library thread starts here: an alternate signal stack first, so a
 * fault on a full stack still reaches the fault report. */
struct bionic_start {
   void *(*fn)(void *);
   void *arg;
};


/* %fs:0 is Android's (and FreeBSD's) TLS self pointer; on the PS5 it is 0 in
 * every thread. Pure mode writes the thread's own struct pthread (%fs:0x10)
 * there, a unique per-thread value, instead of rewriting the engine's code. */
static volatile int bionic_fs_self_on;
static void *
bionic_fs0_read(void)
{
   void *v;
   __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
   return v;
}
static void
bionic_fs_self(void)
{
   if (!bionic_fs_self_on)
      return;
   __asm__ volatile("mov %%fs:0x10, %%rax\n\tmov %%rax, %%fs:0" ::: "rax", "memory");
}

static void *
bionic_thread_start(void *p)
{
   struct bionic_start s = *(struct bionic_start *)p;
   free(p);
   bionic_fs_self();
   stack_t ss;
   ss.ss_size = 256 * 1024;
   ss.ss_sp = malloc(ss.ss_size);
   ss.ss_flags = 0;
   if (ss.ss_sp)
      sigaltstack(&ss, NULL);
   void *r = s.fn(s.arg);
   /* thread_local destructors first (as Bionic does), while the thread's
    * other keys are still set; then the alternate stack goes away. */
   pthread_once(&bionic_tdtor_once, bionic_tdtor_init);
   bionic_tdtor_run(NULL);
   static unsigned ended;
   const unsigned n = __atomic_add_fetch(&ended, 1, __ATOMIC_RELAXED);
   if (n <= 16 || (n & (n - 1)) == 0)
      bionic_say("thread finished (#%u ended so far; thread_local dtors %u registered, %u run)", n,
                 bionic_tdtor_registered, bionic_tdtor_ran);
   if (ss.ss_sp) {
      stack_t off = {0};
      off.ss_flags = SS_DISABLE;
      sigaltstack(&off, NULL);
      free(ss.ss_sp);
   }
   return r;
}

static int
bionic_pthread_create(pthread_t *thread, const struct bionic_attr *battr, void *(*fn)(void *), void *arg)
{
   pthread_attr_t a;
   pthread_attr_init(&a);
   /* Android's sizing: the size the engine asks for, else Bionic's default
    * (1 MiB less a guard page). Stacks come out of the title's small memory
    * pool (mmap fails with ENOMEM at ~350 MiB in all); the old 8 MiB floor
    * spent ~320 MiB of it on 40 threads. ROBLOX_STACK_KB overrides. */
   size_t stack = (1u << 20) - 4096;
   if (battr && battr->stack_size >= 64 * 1024)
      stack = battr->stack_size;
   {
      const char *e = getenv("ROBLOX_STACK_KB");
      const long kb = e ? strtol(e, NULL, 10) : 0;
      if (kb >= 128)
         stack = (size_t)kb << 10;
   }
   stack = (stack + 16383) & ~(size_t)16383; /* whole 16 KiB pages */
   pthread_attr_setstacksize(&a, stack);
   if (battr && (battr->flags & 1u))
      pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
   bionic_threads++;
   bionic_say("pthread_create #%u: stack %zu KiB%s", bionic_threads, stack >> 10,
              battr && (battr->flags & 1u) ? ", detached" : "");
   struct bionic_start *start = malloc(sizeof(*start));
   start->fn = fn;
   start->arg = arg;
   const int rc = pthread_create(thread, &a, bionic_thread_start, start);
   pthread_attr_destroy(&a);
   if (rc == 0) {
      const unsigned k = __atomic_fetch_add(&bionic_thread_list_n, 1, __ATOMIC_RELAXED);
      if (k < sizeof(bionic_thread_list) / sizeof(bionic_thread_list[0]))
         bionic_thread_list[k] = *thread;
   }
   return rc;
}

static int
bionic_setname_np(pthread_t t, const char *name)
{
   (void)t;
   (void)name;
   return 0;
}

static int
bionic_getschedparam(pthread_t t, int *policy, int *param)
{
   (void)t;
   if (policy)
      *policy = 0;
   if (param)
      *param = 0;
   return 0;
}

static int
bionic_setschedparam(pthread_t t, int policy, const int *param)
{
   (void)t;
   (void)policy;
   (void)param;
   return 0;
}

/* ---- locale: one C locale ------------------------------------------------- */

static int bionic_locale_object;
static void *bionic_newlocale(int mask, const char *name, void *base) { (void)mask; (void)name; (void)base; return &bionic_locale_object; }
static void *bionic_uselocale(void *l) { (void)l; return &bionic_locale_object; }
static void bionic_freelocale(void *l) { (void)l; }

/* The locale-aware strto* family: the engine parses flag and cache values with
 * these (the log shows strtoull_l called from inside nativeInitClientSettings,
 * where the flag caches are written). They must not fall through to the caller's
 * trap: a trap returns 0 and leaves the endptr out-parameter untouched, so the
 * caller tests an uninitialised pointer. Parse with the plain function instead
 * of the title's own _l entry point, whose locale object type is not ours. */
static long long bionic_strtoll_l(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
static unsigned long long bionic_strtoull_l(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
static long bionic_strtol_l(const char *s, char **end, int base, void *loc) { (void)loc; return strtol(s, end, base); }
static unsigned long bionic_strtoul_l(const char *s, char **end, int base, void *loc) { (void)loc; return strtoul(s, end, base); }
static double bionic_strtod_l(const char *s, char **end, void *loc) { (void)loc; return strtod(s, end); }
static float bionic_strtof_l(const char *s, char **end, void *loc) { (void)loc; return strtof(s, end); }
static long double bionic_strtold_l(const char *s, char **end, void *loc) { (void)loc; return (long double)strtod(s, end); }


/* ---- memory beyond the title's flexible budget -----------------------------
 * A homebrew title's flexible memory ends near 700 MiB; the logged-in app
 * (images, more threads) needs more and died in bad_alloc. Anonymous
 * mappings the system refuses (ENOMEM) come from direct memory instead -
 * ps5platform's exec layer places it outside the GPU driver's window; regions
 * start read-write (PS5_EXEC_TOGGLED). Huge reservations (>= 256 MiB, the
 * allocator's 1 GiB probes) still fail so it falls back to normal segments.
 * ROBLOX_DMEM_MB caps it (default 2048). */
#if defined(__PROSPERO__)
#include <ps5platform/exec.h>
#include "bionic_dm_pages.h"
#include "bionic_dm_index.h"
#include "bionic_zero.h"
#define BIONIC_DM_MAX 65536
int sceKernelAvailableFlexibleMemorySize(size_t *size);
static struct ps5_exec_region bionic_dm[BIONIC_DM_MAX];
/* Live regions sorted by address (lookups by address), and slots emptied by
 * frees (new regions take one without a walk). Both under bionic_dm_lock. */
static struct bionic_dm_ent bionic_dm_ents[BIONIC_DM_MAX];
static struct bionic_dm_index bionic_dm_idx = {bionic_dm_ents, 0, BIONIC_DM_MAX};
static uint32_t bionic_dm_free_slots[BIONIC_DM_MAX];
static unsigned bionic_dm_free_n;
/* Set when any part of a region may be other than read-write (the engine's
 * mprotect, or a mapping made with another protection). Reusing released
 * pages or a cached region only needs an mprotect back to read-write then;
 * doing it on every reuse was tens of thousands of system calls a second. */
static unsigned char bionic_dm_prot_changed[BIONIC_DM_MAX];
/* The platform rounds direct allocations to 64 KiB. Keep the original mmap
 * length so a partial munmap at the base is not mistaken for a full release. */
static size_t bionic_dm_requested[BIONIC_DM_MAX];
/* Pages released by partial unmaps; the region is freed once all are gone.
 * The counters report memory still held by partly released regions. */
static struct bionic_dm_pages bionic_dm_pages[BIONIC_DM_MAX];
static unsigned char bionic_dm_partial_seen[BIONIC_DM_MAX];
static size_t bionic_dm_partial_bytes;
static unsigned bionic_dm_partial_regions, bionic_dm_piecewise_frees;
/* Cost of this layer, for the frame-rate monitor: calls and time spent mapping
 * (including zeroing) and unmapping direct memory. */
static uint64_t bionic_dm_stat_maps, bionic_dm_stat_unmaps, bionic_dm_stat_frees, bionic_dm_stat_ns;
static uint64_t
bionic_dm_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static unsigned bionic_dm_n;
static size_t bionic_dm_bytes, bionic_dm_cap;
static pthread_mutex_t bionic_dm_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t bionic_dm_in_use(void) { return bionic_dm_bytes; }

/* A new mapping first reuses released pages of a live region (alignment slack
 * the allocator trimmed, pieces it gave back) before asking for more direct
 * memory. Runs start on a console page so protection changes are exact.
 * ROBLOX_DM_REUSE=0 turns this off. */
#define BIONIC_DM_HOLE_ALIGN 16384u
#define BIONIC_DM_HOLE_LOOK 64u
static uint64_t bionic_dm_stat_reused;
static unsigned bionic_dm_hole_hint;
static void *
bionic_dm_take_hole(size_t len, int prot, int *was_changed)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("ROBLOX_DM_REUSE");
      enabled = !(e && e[0] == '0');
   }
   if (!enabled)
      return NULL;
   const size_t n = (len + BIONIC_DM_HOLE_ALIGN - 1) & ~(size_t)(BIONIC_DM_HOLE_ALIGN - 1);
   const uint32_t need = (uint32_t)(n / BIONIC_DM_PAGE);
   uintptr_t got = 0;
   pthread_mutex_lock(&bionic_dm_lock);
   const unsigned count = bionic_dm_n;
   /* Look at no more than BIONIC_DM_HOLE_LOOK slots per map, continuing from
    * where the last search stopped: a full walk of thousands of regions under
    * the lock cost more than the memory it saved. */
   const unsigned look = count < BIONIC_DM_HOLE_LOOK ? count : BIONIC_DM_HOLE_LOOK;
   unsigned k;
   for (k = 0; k < look && !got; k++) {
      const unsigned i = (bionic_dm_hole_hint + k) % count;
      struct bionic_dm_pages *p = &bionic_dm_pages[i];
      if (!bionic_dm[i].base || !p->bits || p->released < need)
         continue;
      got = bionic_dm_pages_take(p, (uintptr_t)bionic_dm[i].base, len, BIONIC_DM_HOLE_ALIGN);
      if (got) {
         bionic_dm_hole_hint = i;
         *was_changed = bionic_dm_prot_changed[i];
         if ((prot & 7) != 3)
            bionic_dm_prot_changed[i] = 1;
      }
   }
   if (!got && count)
      bionic_dm_hole_hint = (bionic_dm_hole_hint + look) % count;
   pthread_mutex_unlock(&bionic_dm_lock);
   if (got)
      __atomic_add_fetch(&bionic_dm_stat_reused, 1, __ATOMIC_RELAXED);
   return (void *)got;
}

/* Wholly released regions wait here before going back to the system: the
 * engine maps and unmaps thousands of same-size regions every second, and
 * reusing one skips the platform allocation and release calls. Cached
 * regions count against the cap and are dropped when it is reached.
 * ROBLOX_DM_CACHE_MB sizes it (default 64, 0 disables). */
#define BIONIC_DM_CACHE_MAX 256
static struct ps5_exec_region bionic_dm_cache[BIONIC_DM_CACHE_MAX];
static unsigned char bionic_dm_cache_prot[BIONIC_DM_CACHE_MAX];
static unsigned bionic_dm_cache_n;
static size_t bionic_dm_cache_bytes, bionic_dm_cache_cap = (size_t)-1;
static uint64_t bionic_dm_stat_cached;
/* Bytes zero-filled for new mappings, and the time that took. */
static uint64_t bionic_dm_stat_zero_bytes, bionic_dm_stat_zero_ns;
/* Time in the hole search, the cache lookup and unmaps (each including
 * waiting for its lock), to show which part of the layer costs what. */
static uint64_t bionic_dm_stat_hole_ns, bionic_dm_stat_cacheget_ns, bionic_dm_stat_unmap_ns;
static void
bionic_dm_zero(void *p, size_t n)
{
   static int stream = -1;
   if (stream < 0) {
      const char *e = getenv("ROBLOX_DM_STREAM");
      stream = !(e && e[0] == '0');
   }
   const uint64_t t = bionic_dm_ns();
   if (stream)
      bionic_zero_fill(p, n);
   else
      memset(p, 0, n);
   __atomic_add_fetch(&bionic_dm_stat_zero_bytes, n, __ATOMIC_RELAXED);
   __atomic_add_fetch(&bionic_dm_stat_zero_ns, bionic_dm_ns() - t, __ATOMIC_RELAXED);
}
static pthread_mutex_t bionic_dm_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* Keep a released region for reuse; 0 if the cache is full (caller frees). */
static int
bionic_dm_cache_put(const struct ps5_exec_region *r, int prot_changed)
{
   if (bionic_dm_cache_cap == (size_t)-1) {
      const char *e = getenv("ROBLOX_DM_CACHE_MB");
      bionic_dm_cache_cap = (size_t)(e ? atoi(e) : 64) << 20;
   }
   int kept = 0;
   pthread_mutex_lock(&bionic_dm_cache_lock);
   if (bionic_dm_cache_n < BIONIC_DM_CACHE_MAX && bionic_dm_cache_bytes + r->bytes <= bionic_dm_cache_cap) {
      bionic_dm_cache_prot[bionic_dm_cache_n] = (unsigned char)prot_changed;
      bionic_dm_cache[bionic_dm_cache_n++] = *r;
      bionic_dm_cache_bytes += r->bytes;
      kept = 1;
   }
   pthread_mutex_unlock(&bionic_dm_cache_lock);
   return kept;
}

/* A cached region the platform would have given for len (same 64 KiB-rounded
 * size), removed from the cache; 0 if none. */
static int
bionic_dm_cache_get(size_t len, struct ps5_exec_region *out, int *prot_changed)
{
   const size_t want = (len + 0xffff) & ~(size_t)0xffff;
   int found = 0;
   pthread_mutex_lock(&bionic_dm_cache_lock);
   for (unsigned i = bionic_dm_cache_n; i-- > 0;) {
      if (bionic_dm_cache[i].bytes == want) {
         *out = bionic_dm_cache[i];
         *prot_changed = bionic_dm_cache_prot[i];
         --bionic_dm_cache_n;
         bionic_dm_cache[i] = bionic_dm_cache[bionic_dm_cache_n];
         bionic_dm_cache_prot[i] = bionic_dm_cache_prot[bionic_dm_cache_n];
         bionic_dm_cache_bytes -= out->bytes;
         found = 1;
         break;
      }
   }
   pthread_mutex_unlock(&bionic_dm_cache_lock);
   return found;
}

/* Return every cached region to the system (the cap was reached). */
static void
bionic_dm_cache_drain(void)
{
   for (;;) {
      struct ps5_exec_region r;
      pthread_mutex_lock(&bionic_dm_cache_lock);
      const int any = bionic_dm_cache_n > 0;
      if (any) {
         r = bionic_dm_cache[--bionic_dm_cache_n];
         bionic_dm_cache_bytes -= r.bytes;
      }
      pthread_mutex_unlock(&bionic_dm_cache_lock);
      if (!any)
         return;
      ps5_exec_free(&r);
   }
}

static void *
bionic_dm_map(size_t len, int prot)
{
   if (!bionic_dm_cap) {
      const char *e = getenv("ROBLOX_DMEM_MB");
      bionic_dm_cap = (size_t)(e ? atoi(e) : 8192) << 20;
   }
   if (len >= (256u << 20))
      return MAP_FAILED;
   const uint64_t t0 = bionic_dm_ns();
   int hole_changed = 0;
   void *hole = bionic_dm_take_hole(len, prot, &hole_changed);
   __atomic_add_fetch(&bionic_dm_stat_hole_ns, bionic_dm_ns() - t0, __ATOMIC_RELAXED);
   if (hole) {
      const size_t n = (len + BIONIC_DM_HOLE_ALIGN - 1) & ~(size_t)(BIONIC_DM_HOLE_ALIGN - 1);
      if (hole_changed)
         mprotect(hole, n, PROT_READ | PROT_WRITE);
      bionic_dm_zero(hole, n);
      if ((prot & 3) != 3)
         mprotect(hole, n, prot & 3);
      __atomic_add_fetch(&bionic_dm_stat_maps, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch(&bionic_dm_stat_ns, bionic_dm_ns() - t0, __ATOMIC_RELAXED);
      return hole;
   }
   struct ps5_exec_region r;
   memset(&r, 0, sizeof(r));
   const uint64_t tc = bionic_dm_ns();
   int cached_changed = 0;
   const int cached = bionic_dm_cache_get(len, &r, &cached_changed);
   __atomic_add_fetch(&bionic_dm_stat_cacheget_ns, bionic_dm_ns() - tc, __ATOMIC_RELAXED);
   if (!cached && bionic_dm_bytes + bionic_dm_cache_bytes + len > bionic_dm_cap)
      bionic_dm_cache_drain();
   if (!cached && bionic_dm_bytes + len > bionic_dm_cap) {
      static unsigned capped;
      if (capped++ < 5)
         bionic_say("MEMORY: direct memory cap reached (%zu MiB of %zu MiB) for %zu bytes", bionic_dm_bytes >> 20,
                    bionic_dm_cap >> 20, len);
      return MAP_FAILED;
   }
   struct ps5_exec_request rq = {len, 0, 0, PS5_EXEC_TOGGLED};
   const int rc = cached ? 0 : ps5_exec_alloc(&rq, &r);
   static unsigned said;
   if (cached) {
      /* the engine may have changed its protection before releasing it */
      if (cached_changed)
         mprotect(r.base, r.bytes, PROT_READ | PROT_WRITE);
      __atomic_add_fetch(&bionic_dm_stat_cached, 1, __ATOMIC_RELAXED);
   }
   if (rc != 0 || !r.base) {
      if (said++ < 10)
         bionic_say("MEMORY: direct memory refused %zu bytes (0x%x), %zu MiB in use", len, (unsigned)rc, bionic_dm_bytes >> 20);
      return MAP_FAILED;
   }
   pthread_mutex_lock(&bionic_dm_lock);
   unsigned i = BIONIC_DM_MAX;
   if (bionic_dm_free_n)
      i = bionic_dm_free_slots[--bionic_dm_free_n];
   else if (bionic_dm_n < BIONIC_DM_MAX)
      i = bionic_dm_n++;
   if (i < BIONIC_DM_MAX) {
      bionic_dm[i] = r;
      bionic_dm_requested[i] = len;
      bionic_dm_partial_seen[i] = 0;
      bionic_dm_prot_changed[i] = (prot & 7) != 3;
      bionic_dm_index_add(&bionic_dm_idx, (uintptr_t)r.base, (uintptr_t)r.base + r.bytes, i);
   }
   bionic_dm_bytes += r.bytes;
   const size_t now = bionic_dm_bytes;
   const size_t partial_bytes = bionic_dm_partial_bytes;
   const unsigned partial_regions = bionic_dm_partial_regions;
   const unsigned piecewise = bionic_dm_piecewise_frees;
   pthread_mutex_unlock(&bionic_dm_lock);
   /* anonymous memory is zero-filled on Linux and the allocator relies on
    * it; direct memory comes back with what was there before */
   bionic_dm_zero(r.base, r.bytes);
   if ((prot & 3) != 3)
      mprotect(r.base, r.bytes, prot & 3);
   __atomic_add_fetch(&bionic_dm_stat_maps, 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&bionic_dm_stat_ns, bionic_dm_ns() - t0, __ATOMIC_RELAXED);
   if (said++ < 10 || (said & 255) == 0)
      bionic_say("dmem: %zu bytes from direct memory at %p (%zu MiB in use)", len, r.base, now >> 20);
   static size_t milestone;
   if (now >= milestone + (128u << 20)) {
      milestone = now & ~(size_t)((128u << 20) - 1);
      bionic_say("MEMORY: direct memory in use passed %zu MiB (%u regions; %zu MiB in %u partly released regions; %u freed piecewise)",
                 milestone >> 20, bionic_dm_n, partial_bytes >> 20, partial_regions, piecewise);
   }
   return r.base;
}

/* -1: not direct memory; 0: handled. Every region the range overlaps records
 * the pages released; a region is freed once all of its pages are, whether by
 * one unmap or several (allocators trim alignment slack, then free the rest). */
static int
bionic_dm_unmap(void *addr, size_t len)
{
   const uintptr_t a = (uintptr_t)addr, e = a + len;
   const uint64_t t0 = bionic_dm_ns();
   int handled = 0;
   unsigned more, freed = 0;
   do {
      /* Free outside the lock, after the walk, so a new mapping at a freed
       * address cannot be visited by this same walk. */
      struct ps5_exec_region gone[16];
      unsigned char gone_prot[16];
      unsigned ngone = 0;
      more = 0;
      pthread_mutex_lock(&bionic_dm_lock);
      for (uint32_t k = bionic_dm_index_first(&bionic_dm_idx, a);
           k < bionic_dm_idx.n && bionic_dm_idx.e[k].base < e; k++) {
         const uint32_t i = bionic_dm_idx.e[k].slot;
         struct ps5_exec_region *r = &bionic_dm[i];
         const uintptr_t b = (uintptr_t)r->base;
         handled = 1;
         if (ngone == 16) {
            more = 1;
            break;
         }
         const int all = bionic_dm_pages_release(&bionic_dm_pages[i], b, bionic_dm_requested[i], a, e);
         if (all == 1) {
            gone_prot[ngone] = bionic_dm_prot_changed[i];
            bionic_dm_prot_changed[i] = 0;
            gone[ngone++] = *r;
            bionic_dm_bytes -= r->bytes;
            if (bionic_dm_partial_seen[i]) {
               bionic_dm_partial_bytes -= r->bytes;
               bionic_dm_partial_regions--;
               bionic_dm_partial_seen[i] = 0;
               bionic_dm_piecewise_frees++;
            }
            bionic_dm_pages_clear(&bionic_dm_pages[i]);
            memset(r, 0, sizeof(*r));
            bionic_dm_requested[i] = 0;
            bionic_dm_index_del_at(&bionic_dm_idx, k--);
            bionic_dm_free_slots[bionic_dm_free_n++] = i;
         } else if (!bionic_dm_partial_seen[i]) {
            bionic_dm_partial_seen[i] = 1;
            bionic_dm_partial_bytes += r->bytes;
            bionic_dm_partial_regions++;
         }
      }
      pthread_mutex_unlock(&bionic_dm_lock);
      for (unsigned k = 0; k < ngone; k++)
         if (!bionic_dm_cache_put(&gone[k], gone_prot[k]))
            ps5_exec_free(&gone[k]);
      freed += ngone;
   } while (more);
   __atomic_add_fetch(&bionic_dm_stat_unmaps, 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&bionic_dm_stat_frees, freed, __ATOMIC_RELAXED);
   __atomic_add_fetch(&bionic_dm_stat_ns, bionic_dm_ns() - t0, __ATOMIC_RELAXED);
   __atomic_add_fetch(&bionic_dm_stat_unmap_ns, bionic_dm_ns() - t0, __ATOMIC_RELAXED);
   return handled ? 0 : -1;
}

/* Totals since start: maps, unmaps seen, regions freed, milliseconds spent,
 * maps served from released pages, maps served from the region cache. */
static void
bionic_dm_stats(uint64_t out[11])
{
   out[0] = __atomic_load_n(&bionic_dm_stat_maps, __ATOMIC_RELAXED);
   out[1] = __atomic_load_n(&bionic_dm_stat_unmaps, __ATOMIC_RELAXED);
   out[2] = __atomic_load_n(&bionic_dm_stat_frees, __ATOMIC_RELAXED);
   out[3] = __atomic_load_n(&bionic_dm_stat_ns, __ATOMIC_RELAXED) / 1000000u;
   out[4] = __atomic_load_n(&bionic_dm_stat_reused, __ATOMIC_RELAXED);
   out[5] = __atomic_load_n(&bionic_dm_stat_cached, __ATOMIC_RELAXED);
   out[6] = __atomic_load_n(&bionic_dm_stat_zero_bytes, __ATOMIC_RELAXED) >> 20;
   out[7] = __atomic_load_n(&bionic_dm_stat_zero_ns, __ATOMIC_RELAXED) / 1000000u;
   out[8] = __atomic_load_n(&bionic_dm_stat_hole_ns, __ATOMIC_RELAXED) / 1000000u;
   out[9] = __atomic_load_n(&bionic_dm_stat_cacheget_ns, __ATOMIC_RELAXED) / 1000000u;
   out[10] = __atomic_load_n(&bionic_dm_stat_unmap_ns, __ATOMIC_RELAXED) / 1000000u;
}

/* Note a protection other than read-write on [addr, addr+len). */
static void
bionic_dm_mark_prot(void *addr, size_t len)
{
   const uintptr_t a = (uintptr_t)addr, e = a + len;
   pthread_mutex_lock(&bionic_dm_lock);
   for (uint32_t k = bionic_dm_index_first(&bionic_dm_idx, a);
        k < bionic_dm_idx.n && bionic_dm_idx.e[k].base < e; k++)
      bionic_dm_prot_changed[bionic_dm_idx.e[k].slot] = 1;
   pthread_mutex_unlock(&bionic_dm_lock);
}

/* MAP_FIXED over released pages of a region makes them live again. */
static void
bionic_dm_reuse(void *addr, size_t len)
{
   const uintptr_t a = (uintptr_t)addr, e = a + len;
   pthread_mutex_lock(&bionic_dm_lock);
   for (uint32_t k = bionic_dm_index_first(&bionic_dm_idx, a);
        k < bionic_dm_idx.n && bionic_dm_idx.e[k].base < e; k++) {
      const uint32_t i = bionic_dm_idx.e[k].slot;
      bionic_dm_pages_reuse(&bionic_dm_pages[i], bionic_dm_idx.e[k].base, bionic_dm_requested[i], a, e);
   }
   pthread_mutex_unlock(&bionic_dm_lock);
}
/* Where direct memory goes: out[0] bytes held, out[1] bytes the engine has
 * released inside regions still held, out[2] the part of those in whole
 * aligned 16 KiB blocks (usable by hole reuse), out[3] regions held,
 * out[4] bytes waiting in the cache. Walks every region: call rarely. */
static void
bionic_dm_waste(uint64_t out[5])
{
   uint64_t released = 0, blocks = 0;
   unsigned regions = 0;
   pthread_mutex_lock(&bionic_dm_lock);
   for (unsigned i = 0; i < bionic_dm_n; i++) {
      if (!bionic_dm[i].base)
         continue;
      regions++;
      released += bionic_dm_pages[i].released;
      blocks += bionic_dm_pages_free_blocks(&bionic_dm_pages[i], BIONIC_DM_HOLE_ALIGN / BIONIC_DM_PAGE);
   }
   out[0] = bionic_dm_bytes;
   pthread_mutex_unlock(&bionic_dm_lock);
   out[1] = released * BIONIC_DM_PAGE;
   out[2] = blocks * BIONIC_DM_HOLE_ALIGN;
   out[3] = regions;
   out[4] = __atomic_load_n(&bionic_dm_cache_bytes, __ATOMIC_RELAXED);
}
#else
static void *bionic_dm_map(size_t len, int prot) { (void)len; (void)prot; return MAP_FAILED; }
static size_t bionic_dm_in_use(void) { return 0; }
static int bionic_dm_unmap(void *addr, size_t len) { (void)addr; (void)len; return -1; }
static void bionic_dm_reuse(void *addr, size_t len) { (void)addr; (void)len; }
static void bionic_dm_mark_prot(void *addr, size_t len) { (void)addr; (void)len; }
static void bionic_dm_stats(uint64_t out[11]) { memset(out, 0, 11 * sizeof(*out)); }
static void bionic_dm_waste(uint64_t out[5]) { memset(out, 0, 5 * sizeof(*out)); }
#endif


static int
bionic_dm_unmap_probe(void *addr)
{
#if defined(__PROSPERO__)
   const uintptr_t a = (uintptr_t)addr;
   pthread_mutex_lock(&bionic_dm_lock);
   const int found = bionic_dm_index_find(&bionic_dm_idx, a) >= 0;
   pthread_mutex_unlock(&bionic_dm_lock);
   return found;
#else
   (void)addr;
   return 0;
#endif
}

/* ---- shared writable file maps ---------------------------------------------
 * The PS5 refuses MAP_SHARED + PROT_WRITE on files (EACCES); the engine uses
 * it for a memory-profiling file (and its UniversalApp cache, which is left
 * failing: see bionic_mmap). Such a small map is emulated:
 * anonymous memory filled from the file, written back to it on msync and
 * munmap. Only this process sees the map, which is all the engine needs. */
#define BIONIC_FMAPS 32
static struct {
   uintptr_t a;
   size_t len;
   int fd; /* our own dup: the engine may close its descriptor first */
   long off;
} bionic_fmap[BIONIC_FMAPS];
static pthread_mutex_t bionic_fmap_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned bionic_fmap_said;

static void *
bionic_fmap_emulate(void *addr, size_t len, int prot, int fixed, int fd, long off)
{
   void *p = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (fixed ? MAP_FIXED : 0), -1, 0);
   if (p == MAP_FAILED)
      return p;
   for (size_t got = 0; got < len;) {
      const ssize_t r = pread(fd, (char *)p + got, len - got, off + (long)got);
      if (r <= 0)
         break;
      got += (size_t)r;
   }
   const int mine = dup(fd);
   pthread_mutex_lock(&bionic_fmap_lock);
   unsigned i;
   for (i = 0; i < BIONIC_FMAPS && bionic_fmap[i].a; i++)
      ;
   if (i < BIONIC_FMAPS && mine >= 0) {
      bionic_fmap[i].a = (uintptr_t)p;
      bionic_fmap[i].len = len;
      bionic_fmap[i].fd = mine;
      bionic_fmap[i].off = off;
   } else if (mine >= 0) {
      close(mine);
   }
   pthread_mutex_unlock(&bionic_fmap_lock);
   if ((prot & 7) != (PROT_READ | PROT_WRITE))
      mprotect(p, len, prot & 7);
   if (bionic_fmap_said++ < 20)
      bionic_say("mmap: shared file map of %zu bytes (fd %d) emulated%s", len, fd,
                 i < BIONIC_FMAPS ? "" : " WITHOUT write-back (table full)");
   return p;
}

/* Write an emulated map back to its file (not past the file's end), and
 * forget it when release is set. Returns 1 if [a, a+n) held one. */
static int
bionic_fmap_flush(void *a, size_t n, int release)
{
   const uintptr_t lo = (uintptr_t)a, hi = lo + n;
   int hit = 0;
   pthread_mutex_lock(&bionic_fmap_lock);
   for (unsigned i = 0; i < BIONIC_FMAPS; i++) {
      if (!bionic_fmap[i].a || bionic_fmap[i].a >= hi || bionic_fmap[i].a + bionic_fmap[i].len <= lo)
         continue;
      hit = 1;
      struct stat st;
      size_t len = bionic_fmap[i].len;
      if (fstat(bionic_fmap[i].fd, &st) == 0) {
         const long room = (long)st.st_size - bionic_fmap[i].off;
         len = room <= 0 ? 0 : (size_t)room < len ? (size_t)room : len;
      }
      for (size_t put = 0; put < len;) {
         const ssize_t w = pwrite(bionic_fmap[i].fd, (const char *)bionic_fmap[i].a + put, len - put,
                                  bionic_fmap[i].off + (long)put);
         if (w <= 0)
            break;
         put += (size_t)w;
      }
      if (release) {
         close(bionic_fmap[i].fd);
         bionic_fmap[i].a = 0;
      }
   }
   pthread_mutex_unlock(&bionic_fmap_lock);
   return hit;
}

static int
bionic_msync(void *a, size_t n, int flags)
{
   (void)flags;
   bionic_fmap_flush(a, n, 0);
   return 0;
}

/* ---- memory: Linux mmap flags to the system's ------------------------------ */

static void *
bionic_mmap(void *addr, size_t len, int prot, int linux_flags, int fd, long off)
{
   int flags = 0;
   if (linux_flags & 0x01) flags |= MAP_SHARED;
   if (linux_flags & 0x02) flags |= MAP_PRIVATE;
   if (linux_flags & 0x10) flags |= MAP_FIXED;
   if (linux_flags & 0x20) flags |= MAP_ANON;
   /* MAP_FIXED over a piece of a direct-memory region (an allocator
    * decommitting): zero it and set the protection, in place */
   if ((linux_flags & 0x30) == 0x30 && addr && bionic_dm_unmap_probe(addr)) {
      bionic_dm_reuse(addr, len);
      if (mprotect(addr, len, PROT_READ | PROT_WRITE) == 0)
         memset(addr, 0, len);
      if ((prot & 3) != 3) {
         mprotect(addr, len, prot & 3);
         bionic_dm_mark_prot(addr, len);
      }
      return addr;
   }
   void *p = MAP_FAILED;
#if defined(__PROSPERO__)
   /* keep flexible memory for the system: the in-game browser (web pages)
    * froze or crashed (CE-108262-9) with ~100 MiB left. Anonymous maps come
    * from direct memory once less than ROBLOX_FLEX_RESERVE_MB (default 300)
    * would remain. */
   if ((linux_flags & 0x20) && !(linux_flags & 0x10) && len >= (64u << 10) && len < (256u << 20)) {
      static size_t reserve = (size_t)-1;
      if (reserve == (size_t)-1) {
         const char *e = getenv("ROBLOX_FLEX_RESERVE_MB");
         reserve = (size_t)(e ? atoi(e) : 300) << 20;
      }
      size_t avail = 0;
      if (reserve && sceKernelAvailableFlexibleMemorySize(&avail) == 0 && avail < reserve + len) {
         p = bionic_dm_map(len, prot);
         static unsigned said_reserve;
         if (p != MAP_FAILED && said_reserve++ < 3)
            bionic_say("MEMORY: %zu MiB flexible left: %zu bytes from direct memory instead", avail >> 20, len);
      }
   }
   if (p == MAP_FAILED)
#endif
      p = mmap(addr, len, prot & 7, flags, (linux_flags & 0x20) ? -1 : fd, off);
   /* small ones only: the UniversalApp cache (100 MB) is written through
    * its map and renamed while still mapped, so emulating it left an empty
    * cache file whose checksum matched - the next start hung. Failing its
    * map just makes the engine skip the cache. */
   if (p == MAP_FAILED && errno == EACCES && len <= (1u << 20) && (linux_flags & 0x01) && !(linux_flags & 0x20) && fd >= 0 &&
       (prot & PROT_WRITE))
      p = bionic_fmap_emulate(addr, len, prot, linux_flags & 0x10, fd, off);
   if (p == MAP_FAILED && errno == ENOMEM && (linux_flags & 0x20) && !(linux_flags & 0x10))
      p = bionic_dm_map(len, prot);
   static unsigned said, failed;
   static size_t total;
   if (p != MAP_FAILED)
      total += len;
   if (p == MAP_FAILED && len != 1073807360) {
      if (failed++ < 40)
         bionic_say("MEMORY: a map of %zu bytes (prot %d, flags 0x%x, fd %d, offset %ld) FAILED, errno %d (mapped so far ~%zu MiB, direct %zu MiB)",
                    len, prot, linux_flags, fd, off, errno, total >> 20, bionic_dm_in_use() >> 20);
   } else if (said++ < 300) {
      bionic_say("mmap(%p, %zu, prot %d, flags 0x%x) = %p", addr, len, prot, linux_flags, p);
   }
   return p;
}

/* The PS5 unmaps and protects whole 16 KiB pages; code written for 4 KiB
 * pages may release or protect a 4 KiB piece next to live data. Releasing and
 * removing access only touch the whole 16 KiB pages inside the range; granting
 * access covers every page the range touches. */
#define BIONIC_PAGE 16384ul
static unsigned bionic_partial_unmaps, bionic_partial_protects;
static int
bionic_munmap(void *a, size_t n)
{
   bionic_fmap_flush(a, n, 1);
   if (bionic_dm_unmap(a, n) == 0)
      return 0;
   uintptr_t lo = ((uintptr_t)a + BIONIC_PAGE - 1) & ~(BIONIC_PAGE - 1);
   uintptr_t hi = ((uintptr_t)a + n) & ~(BIONIC_PAGE - 1);
   if (lo != (uintptr_t)a || hi != (uintptr_t)a + n) {
      if (bionic_partial_unmaps++ < 5)
         bionic_say("munmap(%p, %zu): only the whole 16 KiB pages inside are released", a, n);
   }
   bionic_say("munmap(%p, %zu) -> [%p,%p)", a, n, (void *)lo, (void *)hi);
   return hi > lo ? munmap((void *)lo, hi - lo) : 0;
}
/* The range is always rounded INWARD (to whole 16 KiB pages inside it). The
 * engine is built for 4 KiB pages: rounding outward would re-protect the
 * neighbouring 16 KiB page, and on the PS5 mprotect cannot give execute back
 * once it is gone - that silently killed the first code page of the library.
 * Rounding inward may leave a 4 KiB piece unprotected, which only loses a
 * guard page, never permissions the code needs. */
static int
bionic_mprotect(void *a, size_t n, int prot)
{
   prot &= 7;
   const uintptr_t lo = ((uintptr_t)a + BIONIC_PAGE - 1) & ~(BIONIC_PAGE - 1);
   const uintptr_t hi = ((uintptr_t)a + n) & ~(BIONIC_PAGE - 1);
   static unsigned said;
   if (lo != (uintptr_t)a || hi != (uintptr_t)a + n) {
      if (said++ < 10)
         bionic_say("mprotect(%p, %zu, %d) -> only the whole pages inside: [%p,%p)", a, n, prot, (void *)lo,
                    (void *)hi);
   }
   if (hi > lo && prot != (PROT_READ | PROT_WRITE))
      bionic_dm_mark_prot((void *)lo, hi - lo);
   return hi > lo ? mprotect((void *)lo, hi - lo, prot) : 0;
}
/* Linux MADV_DONTNEED (4) on private anonymous memory: the pages read back
 * as zeros afterwards, and allocators rely on it for memory they reuse. */
static unsigned bionic_dontneed_calls;
static int
bionic_madvise(void *a, size_t n, int advice)
{
   if (advice != 4 && advice != 9 /* MADV_REMOVE */)
      return 0;
   if (bionic_dontneed_calls++ < 60)
      bionic_say("madvise(%p, %zu, %d)", a, n, advice);
   const uintptr_t start = (uintptr_t)a, end = start + n;
   const uintptr_t lo = (start + BIONIC_PAGE - 1) & ~(BIONIC_PAGE - 1), hi = end & ~(BIONIC_PAGE - 1);
   /* direct memory (bionic_dm_map) must not be mapped over: the region stays
    * the engine's (mimalloc reuses purged arena pages); zero it in place */
   if (bionic_dm_unmap_probe(a)) {
      bionic_dm_zero(a, n);
      return 0;
   }
   if (hi > lo) {
      if (mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
         memset((void *)lo, 0, hi - lo);
      if (lo > start)
         memset((void *)start, 0, lo - start);
      if (end > hi)
         memset((void *)hi, 0, end - hi);
   } else {
      memset(a, 0, n);
   }
   return 0;
}

/* ---- system queries --------------------------------------------------------- */

static long
bionic_sysconf(int name)
{
   /* Bionic's own numbering (libc/include/bits/sysconf.h), not glibc's. */
   switch (name) {
   case 0x27: /* _SC_PAGESIZE */
   case 0x28: /* _SC_PAGE_SIZE */ return 16384; /* the PS5 maps and protects 16 KiB pages */
   case 0x60: /* _SC_NPROCESSORS_CONF */
   case 0x61: /* _SC_NPROCESSORS_ONLN */ {
      /* ROBLOX_CPUS=n (diagnostic): report fewer cores, so fewer workers */
      const char *c = getenv("ROBLOX_CPUS");
      const long n = c ? strtol(c, NULL, 10) : 0;
      return n > 0 && n <= 8 ? n : 8;
   }
   case 0x62: /* _SC_PHYS_PAGES */ return (long)((4ull << 30) / 16384);
   case 0x63: /* _SC_AVPHYS_PAGES */ return (long)((2ull << 30) / 16384);
   case 0x06: /* _SC_CLK_TCK */ return 100;
   case 0x0b: /* _SC_OPEN_MAX */ return 1024;
   case 0x00: /* _SC_ARG_MAX */ return 131072;
   case 0x4c: /* _SC_THREAD_STACK_MIN */ return 16384;
   case 0x94: /* _SC_LEVEL1_DCACHE_LINESIZE */ return 64;
   default: {
      static uint8_t warned[256];
      if (name >= 0 && name < 256 && !warned[name]) {
         warned[name] = 1;
         bionic_say("sysconf(0x%x) is not mapped; returning -1", name);
      }
      errno = EINVAL;
      return -1;
   }
   }
}

static int bionic_getpagesize(void) { return 16384; }
static unsigned long
bionic_getauxval(unsigned long type)
{
   switch (type) {
   case 6: return 4096;       /* AT_PAGESZ */
   case 16: return 0x178bfbff; /* AT_HWCAP */
   case 26: return 2;         /* AT_HWCAP2 */
   case 17: return 100;       /* AT_CLKTCK */
   case 23: return 0;         /* AT_SECURE */
   default: return 0;
   }
}
static int bionic_gettid(void) { return (int)(uintptr_t)pthread_self() & 0x7fffffff; }
static int bionic_prctl(int op, ...) { (void)op; return 0; }
static int bionic_getentropy(void *buf, size_t n);

/* Linux futexes on condition variables: a word's waiters sleep on the
 * condition of the bucket its address hashes to; a wake broadcasts there
 * (futex waiters must tolerate spurious wakeups, so that is correct). Errors
 * use Linux numbers, as the library reads them: EAGAIN 11, ETIMEDOUT 110. */
#define BIONIC_FUTEX_BUCKETS 64
static pthread_mutex_t bionic_futex_lock[BIONIC_FUTEX_BUCKETS];
static pthread_cond_t bionic_futex_cond[BIONIC_FUTEX_BUCKETS];
static pthread_once_t bionic_futex_once = PTHREAD_ONCE_INIT;
static void
bionic_futex_init(void)
{
   for (int i = 0; i < BIONIC_FUTEX_BUCKETS; i++) {
      pthread_mutex_init(&bionic_futex_lock[i], NULL);
      pthread_condattr_t a;
      pthread_condattr_init(&a);
      pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
      pthread_cond_init(&bionic_futex_cond[i], &a);
      pthread_condattr_destroy(&a);
   }
}

static long
bionic_futex(volatile int *addr, int op, int val, const struct timespec *timeout, int bitset_val3)
{
   (void)bitset_val3;
   pthread_once(&bionic_futex_once, bionic_futex_init);
   const unsigned b = (unsigned)(((uintptr_t)addr >> 2) * 2654435761u) % BIONIC_FUTEX_BUCKETS;
   const int cmd = op & 0x7f; /* without FUTEX_PRIVATE_FLAG / CLOCK_REALTIME */
   if (cmd == 0 || cmd == 9) { /* WAIT, WAIT_BITSET */
      pthread_mutex_lock(&bionic_futex_lock[b]);
      if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) {
         pthread_mutex_unlock(&bionic_futex_lock[b]);
         errno = 11;
         return -1;
      }
      int rc = 0;
      if (timeout) {
         struct timespec until;
         if (cmd == 9) {
            until = *timeout; /* WAIT_BITSET: absolute, monotonic unless REALTIME flag */
         } else {
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_sec += timeout->tv_sec;
            until.tv_nsec += timeout->tv_nsec;
            if (until.tv_nsec >= 1000000000L) {
               until.tv_sec++;
               until.tv_nsec -= 1000000000L;
            }
         }
         rc = pthread_cond_timedwait(&bionic_futex_cond[b], &bionic_futex_lock[b], &until);
      } else {
         rc = pthread_cond_wait(&bionic_futex_cond[b], &bionic_futex_lock[b]);
      }
      pthread_mutex_unlock(&bionic_futex_lock[b]);
      if (rc == ETIMEDOUT) {
         errno = 110;
         return -1;
      }
      return 0;
   }
   if (cmd == 1 || cmd == 10) { /* WAKE, WAKE_BITSET */
      pthread_mutex_lock(&bionic_futex_lock[b]);
      pthread_cond_broadcast(&bionic_futex_cond[b]);
      pthread_mutex_unlock(&bionic_futex_lock[b]);
      return val;
   }
   static int warned;
   if (!warned++)
      bionic_say("futex op %d is not provided", op);
   errno = 38; /* ENOSYS */
   return -1;
}

static long
bionic_syscall(long n, ...)
{
   va_list ap;
   va_start(ap, n);
   const long a0 = va_arg(ap, long), a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
   va_arg(ap, long);
   const long a5 = va_arg(ap, long);
   va_end(ap);
   switch (n) {
   case 202: /* futex(addr, op, val, timeout, addr2, val3) */
      return bionic_futex((volatile int *)a0, (int)a1, (int)a2, (const struct timespec *)a3, (int)a5);
   case 186: /* gettid */ return (long)((uintptr_t)pthread_self() & 0x7fffffff);
   case 39: /* getpid */ return (long)getpid();
   case 318: /* getrandom(buf, len, flags) */
      bionic_getentropy((void *)a0, (size_t)a1);
      return a1;
   default: {
      static unsigned char warned[512];
      if (n < 0 || n >= 512 || !warned[n]) {
         if (n >= 0 && n < 512)
            warned[n] = 1;
         bionic_say("syscall(%ld) is not provided", n);
      }
      errno = 38;
      return -1;
   }
   }
}
static int bionic_sched_getcpu(void) { return 0; }

static int
bionic_system_property_get(const char *name, char *value)
{
   /* consistent with the JNI side: Android 13 (SDK 33), a release build */
   static const char *const props[][2] = {
      {"ro.build.version.sdk", "33"},
      {"ro.build.version.release", "13"},
      {"ro.build.version.codename", "REL"},
      {"ro.product.model", "PlayStation 5"},
      {"ro.product.manufacturer", "Sony"},
      {"ro.product.brand", "Sony"},
      {"ro.product.device", "ps5"},
      {"ro.product.name", "ps5"},
      {"ro.hardware", "ps5"},
      {"ro.board.platform", "oberon"},
      {"ro.product.cpu.abi", "x86_64"},
      {"ro.product.cpu.abilist", "x86_64"},
      {"ro.product.cpu.abilist64", "x86_64"},
      {"ro.build.fingerprint", "Sony/ps5/ps5:13/TQ3A.230901.001/1:user/release-keys"},
      {"ro.build.type", "user"},
      {"ro.build.tags", "release-keys"},
      {"ro.debuggable", "0"},
      {"ro.secure", "1"},
      {"ro.kernel.qemu", "0"},
   };
   value[0] = '\0';
   for (unsigned i = 0; name && i < sizeof(props) / sizeof(props[0]); i++)
      if (strcmp(name, props[i][0]) == 0) {
         snprintf(value, 92, "%s", props[i][1]); /* PROP_VALUE_MAX */
         return (int)strlen(value);
      }
   static unsigned said;
   if (name && said++ < 24)
      bionic_say("property %s: (none)", name);
   return 0;
}

static int
bionic_getentropy(void *buf, size_t n)
{
   static uint64_t x = 0x9e3779b97f4a7c15ull;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   x ^= (uint64_t)ts.tv_nsec * 0xbf58476d1ce4e5b9ull;
   uint8_t *p = buf;
   for (size_t i = 0; i < n; i++) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      p[i] = (uint8_t)x;
   }
   return 0;
}

static void bionic_arc4random_buf(void *buf, size_t n) { bionic_getentropy(buf, n); }

/* ---- _FORTIFY_SOURCE entry points ----------------------------------------- */

static size_t bionic_fread_chk(void *buf, size_t bufsz, size_t size, size_t count, FILE *f) { (void)bufsz; return fread(buf, size, count, f); }
static size_t bionic_fwrite_chk(const void *buf, size_t bufsz, size_t size, size_t count, FILE *f) { (void)bufsz; return fwrite(buf, size, count, f); }
static int bionic_vsnprintf_chk(char *b, size_t n, int flags, size_t bn, const char *fmt, va_list ap) { (void)flags; (void)bn; return vsnprintf(b, n, fmt, ap); }
static int bionic_vsprintf_chk(char *b, int flags, size_t bn, const char *fmt, va_list ap) { (void)flags; return vsnprintf(b, bn, fmt, ap); }
static char *bionic_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) { (void)dn; (void)sn; return strncpy(d, s, n); }
static ssize_t bionic_write_chk(int fd, const void *b, size_t n, size_t bn) { (void)bn; return write(fd, b, n); }
static ssize_t bionic_readlink_chk(const char *p, char *b, size_t n, size_t bn) { (void)bn; return readlink(p, b, n); }
static int
bionic_gnu_strerror_r(int e, char *buf, size_t n)
{
   snprintf(buf, n, "%s", strerror(e));
   return 0;
}

/* ---- dl_iterate_phdr: libunwind finds libroblox's .eh_frame_hdr here ------- */

struct bionic_dl_phdr_info {
   uintptr_t dlpi_addr;
   const char *dlpi_name;
   const void *dlpi_phdr;
   uint16_t dlpi_phnum;
   unsigned long long dlpi_adds, dlpi_subs;
   size_t dlpi_tls_modid;
   void *dlpi_tls_data;
};

static uintptr_t bionic_lib_base;
static const void *bionic_lib_phdr;
static uint16_t bionic_lib_phnum;

static int
bionic_dl_iterate_phdr(int (*cb)(struct bionic_dl_phdr_info *, size_t, void *), void *data)
{
   if (!bionic_lib_base)
      return 0;
   struct bionic_dl_phdr_info info = {
      .dlpi_addr = bionic_lib_base,
      .dlpi_name = "libroblox.so",
      .dlpi_phdr = bionic_lib_phdr,
      .dlpi_phnum = bionic_lib_phnum,
      .dlpi_adds = 1,
   };
   return cb(&info, sizeof(info), data);
}

static const struct ael_import *bionic_table; /* bionic_imports, set by bionic_shims_init */
static int bionic_libc_handle;

/* dlopen of the system libraries gives one handle whose dlsym answers from
 * this file's own table, as Bionic's libc.so/libm.so/libdl.so would. */
/* libvulkan.so: answered by the title (its RADV) when it sets this hook */
static void *(*bionic_vulkan_dlsym)(const char *name);
static int bionic_vulkan_handle;

static void *
bionic_dlopen(const char *name, int flags)
{
   if (name && strstr(name, "libvulkan.so") && bionic_vulkan_dlsym) {
      bionic_say("dlopen(%s, %d): the title's Vulkan (RADV)", name, flags);
      return &bionic_vulkan_handle;
   }
   if (name && (strstr(name, "libc.so") || strstr(name, "libm.so") || strstr(name, "libdl.so") ||
                strstr(name, "liblog.so")))
      return &bionic_libc_handle;
   bionic_say("dlopen(%s, %d): not provided", name ? name : "NULL", flags);
   return NULL;
}
static const char *bionic_dlerror(void) { return "this library is not provided on the PS5 yet"; }
static void *
bionic_dlsym(void *h, const char *name)
{
   if (h == &bionic_vulkan_handle && bionic_vulkan_dlsym)
      return bionic_vulkan_dlsym(name);
   if (name && bionic_table)
      for (const struct ael_import *i = bionic_table; i->name; i++)
         if (strcmp(i->name, name) == 0)
            return i->address;
   bionic_say("dlsym(%s, %s): not provided", h == &bionic_libc_handle ? "libc" : "?", name ? name : "NULL");
   return NULL;
}
static int bionic_dlclose(void *h) { (void)h; return 0; }
static int bionic_dladdr(const void *a, void *info) { (void)a; (void)info; return 0; }

/* exit / _exit / _Exit / abort from the library: say who called, then park
 * the calling thread instead of ending the title, so the log survives and the
 * rest keeps running. */
static void
bionic_park(const char *what, int code, void *caller)
{
   const uintptr_t c = (uintptr_t)caller;
   bionic_say("%s(%d) called from %s+0x%lx; thread parked", what, code,
              c >= bionic_lib_base && bionic_lib_base ? "libroblox.so" : "outside",
              (unsigned long)(c >= bionic_lib_base && bionic_lib_base ? c - bionic_lib_base : c));
   for (;;)
      sleep(60);
}

static void bionic_exit(int code) { bionic_park("exit", code, __builtin_return_address(0)); }
static void bionic__exit(int code) { bionic_park("_exit", code, __builtin_return_address(0)); }
static void bionic_abort(void) { bionic_park("abort", 0, __builtin_return_address(0)); }
static void bionic_pthread_exit(void *v) { (void)v; bionic_park("pthread_exit", 0, __builtin_return_address(0)); }

/* ---- files: Linux open flags and struct stat ------------------------------ */

#include <fcntl.h>
#include <sys/stat.h>
#include <ps5platform/libc.h>

/* Opt-in experiment: track directory descriptors for raw openat. */
static int bionic_track_openat;

static int
bionic_open_flags(int lf)
{
   int f = lf & 3; /* O_RDONLY / O_WRONLY / O_RDWR are the same */
   if (lf & 0x40) f |= O_CREAT;
   if (lf & 0x80) f |= O_EXCL;
   if (lf & 0x100) f |= O_NOCTTY;
   if (lf & 0x200) f |= O_TRUNC;
   if (lf & 0x400) f |= O_APPEND;
   if (lf & 0x800) f |= O_NONBLOCK;
#ifdef O_DIRECTORY
   if (lf & 0x10000) f |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
   if (lf & 0x20000) f |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
   if (lf & 0x80000) f |= O_CLOEXEC;
#endif
   return f;
}

/* Linux-only trees the client probes; there is nothing like them here. */
static bool
bionic_path_absent(const char *path)
{
   return !path || strncmp(path, "/proc/", 6) == 0 || strncmp(path, "/sys/", 5) == 0 ||
          strncmp(path, "/system/", 8) == 0 || strncmp(path, "/dev/", 5) == 0;
}

/* /dev/urandom and /dev/random: a descriptor number no real file gets, whose
 * reads come from bionic_getentropy. */
enum { BIONIC_URANDOM_FD = 0x7ff0 };

/* /proc files the engine polls (its memory monitor opens /proc/meminfo and
 * /proc/self/statm several times a second): small generated answers. Linux
 * reports /proc sizes in 4 KiB pages. */
#define BIONIC_LINUX_PAGE 4096
enum { BIONIC_PROC_BASE_FD = 0x7f00, BIONIC_PROC_FDS = 8 };
static struct {
   int used;
   size_t len, pos;
   char data[2048];
} bionic_proc[BIONIC_PROC_FDS];

static pthread_mutex_t bionic_proc_lock = PTHREAD_MUTEX_INITIALIZER;
static int
bionic_proc_open(const char *path, const char *what)
{
   (void)what;
   int slot = -1;
   pthread_mutex_lock(&bionic_proc_lock);
   for (int i = 0; i < BIONIC_PROC_FDS; i++)
      if (!bionic_proc[i].used) {
         bionic_proc[i].used = 1;
         slot = i;
         break;
      }
   pthread_mutex_unlock(&bionic_proc_lock);
   if (slot < 0) {
      errno = EMFILE;
      return -1;
   }
   char *d = bionic_proc[slot].data;
   const size_t cap = sizeof(bionic_proc[slot].data);
   if (strcmp(path, "/proc/meminfo") == 0) {
      /* A steady picture: the console dedicates ~8 GiB to the game. */
      snprintf(d, cap,
               "MemTotal:        8388608 kB\n"
               "MemFree:         5242880 kB\n"
               "MemAvailable:    6291456 kB\n"
               "Buffers:          262144 kB\n"
               "Cached:           786432 kB\n"
               "SwapTotal:             0 kB\n"
               "SwapFree:              0 kB\n"
               "VmallocTotal:   34359738367 kB\n"
               "VmallocUsed:        4096 kB\n");
   } else if (strcmp(path, "/proc/self/statm") == 0) {
      /* size resident shared text lib data dt, in Linux pages */
      snprintf(d, cap, "%lu %lu 24576 65536 0 %lu 0\n", (unsigned long)(512 << 20) / BIONIC_LINUX_PAGE,
               (unsigned long)(320 << 20) / BIONIC_LINUX_PAGE,
               (unsigned long)(320 << 20) / BIONIC_LINUX_PAGE - 24576);
   } else if (strcmp(path, "/proc/cpuinfo") == 0) {
      snprintf(d, cap,
               "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD custom APU 0x4f20\n");
   } else if (strcmp(path, "/proc/sys/vm/overcommit_memory") == 0) {
      snprintf(d, cap, "0\n");
   } else {
      bionic_proc[slot].used = 0;
      errno = ENOENT;
      return -1;
   }
   bionic_proc[slot].len = strlen(d);
   bionic_proc[slot].pos = 0;
   return BIONIC_PROC_BASE_FD + slot;
}

/* eventfd table (the shims are further down) */
#define BIONIC_EFD_MAX 64
static struct { int rfd, wfd, sem; } bionic_efd[BIONIC_EFD_MAX];
static ssize_t bionic_efd_read(int slot, void *buf, size_t n);

static ssize_t
bionic_read(int fd, void *buf, size_t n)
{
   if (fd == BIONIC_URANDOM_FD) {
      bionic_getentropy(buf, n);
      return (ssize_t)n;
   }
   if (fd >= BIONIC_PROC_BASE_FD && fd < BIONIC_PROC_BASE_FD + BIONIC_PROC_FDS) {
      const int i = fd - BIONIC_PROC_BASE_FD;
      if (bionic_proc[i].pos >= bionic_proc[i].len)
         return 0;
      const size_t left = bionic_proc[i].len - bionic_proc[i].pos;
      const size_t take = n < left ? n : left;
      memcpy(buf, bionic_proc[i].data + bionic_proc[i].pos, take);
      bionic_proc[i].pos += take;
      return (ssize_t)take;
   }
   {
      int slot = -1;
      for (int i = 0; i < BIONIC_EFD_MAX; i++)
         if (bionic_efd[i].rfd == fd && bionic_efd[i].wfd > 0)
            slot = i;
      if (slot >= 0)
         return bionic_efd_read(slot, buf, n);
   }
   return read(fd, buf, n);
}

static ssize_t bionic_read_chk2(int fd, void *b, size_t n, size_t bn) { (void)bn; return bionic_read(fd, b, n); }

static void bn_forget(int fd);
static int
bionic_close(int fd)
{
   bn_forget(fd);
   if (fd == BIONIC_URANDOM_FD)
      return 0;
   if (fd >= BIONIC_PROC_BASE_FD && fd < BIONIC_PROC_BASE_FD + BIONIC_PROC_FDS) {
      bionic_proc[fd - BIONIC_PROC_BASE_FD].used = 0;
      return 0;
   }
   for (int i = 0; i < BIONIC_EFD_MAX; i++)
      if (bionic_efd[i].rfd == fd && bionic_efd[i].wfd > 0) {
         close(bionic_efd[i].wfd);
         bionic_efd[i].wfd = 0;
         bionic_efd[i].rfd = 0;
      }
   return close(fd);
}

static int
bionic_strerror_r(int e, char *buf, size_t n)
{
   snprintf(buf, n, "%s", strerror(e));
   return 0;
}

static void bionic_openlog(const char *ident, int opt, int facility) { (void)ident; (void)opt; (void)facility; }
static void bionic_closelog(void) {}

static void
bionic_syslog(int prio, const char *fmt, ...)
{
   char text[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);
   bionic_say("[syslog %d] %s", prio, text);
}


/* Relative paths are relative to the app's files directory: the title's own
 * working directory (/app0) is read-only and chdir is not allowed. */
#define BIONIC_FILES_DIR "/data/roblox/files"
static const char *
bionic_path(const char *path, char *buf, size_t n)
{
   if (!path || path[0] == '/' || path[0] == '\0')
      return path;
   while (path[0] == '.' && path[1] == '/')
      path += 2;
   snprintf(buf, n, "%s/%s", BIONIC_FILES_DIR, path[0] == '.' && !path[1] ? "" : path);
   return buf;
}
#define BIONIC_PATH(p) char p##_buf[1024]; p = bionic_path(p, p##_buf, sizeof(p##_buf))


/* ---- /proc files an Android process can read about itself ------------------
 * Generated as real files under /data/roblox/proc (so open, fopen, read,
 * fstat and mmap all work), describing this process as it is: where the
 * engine's library is mapped and from which file (the unmodified
 * libroblox.so of the official APK, at /app0/libroblox.so), its thread
 * stacks, the auxiliary vector, the process name. Real Android has these;
 * without them the engine cannot find its own files. ROBLOX_NO_SELFPROC=1
 * keeps the old "not on this system". */
#define BIONIC_SELF_LIB "/app0/libroblox.so"
static const char *
bionic_selfproc(const char *path, char *out, size_t outsz)
{
   if (!path || getenv("ROBLOX_NO_SELFPROC"))
      return NULL;
   const char *leaf = NULL;
   if (strncmp(path, "/proc/self/", 11) == 0)
      leaf = path + 11;
   else if (strncmp(path, "/proc/thread-self/", 18) == 0)
      leaf = path + 18;
   else if (strncmp(path, "/proc/", 6) == 0 && path[6] >= '0' && path[6] <= '9') {
      const char *sl = strchr(path + 6, '/');
      if (!sl || atoi(path + 6) != (int)getpid())
         return NULL;
      leaf = sl + 1;
   } else if (strcmp(path, "/proc/net/unix") == 0) {
      leaf = "net/unix";
   } else if (strcmp(path, "/proc/mounts") == 0) {
      leaf = "mounts";
   }
   if (!leaf)
      return NULL;
   static const char *const known[] = {"maps", "smaps", "auxv", "mounts", "cmdline", "status", "stat", "comm",
                                       "net/unix", "environ", NULL};
   int k;
   for (k = 0; known[k]; k++)
      if (strcmp(leaf, known[k]) == 0)
         break;
   if (!known[k])
      return NULL;
   mkdir("/data/roblox/proc", 0777);
   char name[64];
   snprintf(name, sizeof(name), "%s", leaf);
   for (char *c = name; *c; c++)
      if (*c == '/')
         *c = '_';
   snprintf(out, outsz, "/data/roblox/proc/%s", name);
   /* written once per run, then reused: the engine polls stat/status several
    * times a second, and a file write each time cost most of the frame rate */
   static uint32_t made; /* bit per known[] entry */
   static pthread_mutex_t made_lock = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&made_lock);
   if (made & (1u << k)) {
      pthread_mutex_unlock(&made_lock);
      return out;
   }
   FILE *f = fopen(out, "wb");
   if (!f) {
      pthread_mutex_unlock(&made_lock);
      return NULL;
   }
   const pid_t pid = getpid();
   if (strcmp(leaf, "maps") == 0 || strcmp(leaf, "smaps") == 0) {
      /* the library's segments, in address order, with their file offsets */
      const struct { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; } *ph = bionic_lib_phdr;
      struct stat st;
      const unsigned long ino = stat(BIONIC_SELF_LIB, &st) == 0 ? (unsigned long)st.st_ino : 0;
      for (unsigned i = 0; ph && i < bionic_lib_phnum; i++) {
         if (ph[i].type != 1)
            continue;
         const uintptr_t lo = (bionic_lib_base + ph[i].vaddr) & ~0xfffull;
         const uintptr_t hi = (bionic_lib_base + ph[i].vaddr + ph[i].memsz + 0xfff) & ~0xfffull;
         const uintptr_t filehi = (bionic_lib_base + ph[i].vaddr + ph[i].filesz + 0xfff) & ~0xfffull;
         const char perm[5] = {(ph[i].flags & 4) ? 'r' : '-', (ph[i].flags & 2) ? 'w' : '-',
                               (ph[i].flags & 1) ? 'x' : '-', 'p', 0};
         fprintf(f, "%012lx-%012lx %s %08lx fe:2a %lu                         %s\n", (unsigned long)lo,
                 (unsigned long)(filehi < hi ? filehi : hi), perm, (unsigned long)(ph[i].off & ~0xfffull), ino,
                 BIONIC_SELF_LIB);
         if (filehi < hi)
            fprintf(f, "%012lx-%012lx %s 00000000 00:00 0                          [anon:.bss]\n",
                    (unsigned long)filehi, (unsigned long)hi, perm);
      }
      fprintf(f, "7ffd1c000000-7ffd1c021000 rw-p 00000000 00:00 0                          [stack]\n");
   } else if (strcmp(leaf, "auxv") == 0) {
      static uint8_t rnd[16];
      static const char plat[] = "x86_64";
      if (!rnd[0])
         bionic_getentropy(rnd, sizeof(rnd));
      const uint64_t v[] = {6,  4096,                         /* AT_PAGESZ */
                            16, 0x178bfbff,                   /* AT_HWCAP */
                            17, 100,                          /* AT_CLKTCK */
                            11, 10143, 12, 10143, 13, 10143, 14, 10143, /* uid/euid/gid/egid */
                            23, 0,                            /* AT_SECURE */
                            25, (uint64_t)(uintptr_t)rnd,     /* AT_RANDOM */
                            26, 2,                            /* AT_HWCAP2 */
                            15, (uint64_t)(uintptr_t)plat,    /* AT_PLATFORM */
                            0,  0};
      fwrite(v, 1, sizeof(v), f);
   } else if (strcmp(leaf, "mounts") == 0) {
      fprintf(f, "/dev/block/dm-6 / ext4 ro,seclabel,relatime 0 0\n"
                 "tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,mode=755 0 0\n"
                 "proc /proc proc rw,relatime,gid=3009,hidepid=invisible 0 0\n"
                 "sysfs /sys sysfs rw,seclabel,relatime 0 0\n"
                 "/dev/block/dm-48 /data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime 0 0\n");
   } else if (strcmp(leaf, "cmdline") == 0) {
      fwrite("com.roblox.client", 1, sizeof("com.roblox.client"), f);
   } else if (strcmp(leaf, "comm") == 0) {
      fprintf(f, "com.roblox.clie\n");
   } else if (strcmp(leaf, "status") == 0) {
      fprintf(f,
              "Name:\tcom.roblox.clie\nUmask:\t0077\nState:\tS (sleeping)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\n"
              "PPid:\t1\nTracerPid:\t0\nUid:\t10143\t10143\t10143\t10143\nGid:\t10143\t10143\t10143\t10143\n"
              "FDSize:\t256\nVmPeak:\t 8000000 kB\nVmSize:\t 7800000 kB\nVmRSS:\t  900000 kB\nThreads:\t80\n"
              "SigQ:\t0/31335\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
              "Seccomp:\t2\nSeccomp_filters:\t1\nCpus_allowed:\tff\nCpus_allowed_list:\t0-7\n",
              (int)pid, (int)pid);
   } else if (strcmp(leaf, "stat") == 0) {
      fprintf(f, "%d (com.roblox.clie) S 1 1 0 0 -1 1077952832 0 0 0 0 100 50 0 0 20 0 80 0 1000 7800000000 "
                 "225000 18446744073709551615 0 0 0 0 0 0 4612 1 1073775864 0 0 0 17 3 0 0 0 0 0\n",
              (int)pid);
   } else if (strcmp(leaf, "net/unix") == 0) {
      fprintf(f, "Num       RefCount Protocol Flags    Type St Inode Path\n");
   }
   fclose(f);
   made |= 1u << k;
   pthread_mutex_unlock(&made_lock);
   bionic_say("selfproc: %s generated", path);
   return out;
}

static int
bionic_open(const char *path, int linux_flags, ...)
{
   BIONIC_PATH(path);
   int mode = 0;
   if (linux_flags & 0x40) {
      va_list ap;
      va_start(ap, linux_flags);
      mode = va_arg(ap, int);
      va_end(ap);
   }
   if (path && (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0))
      return BIONIC_URANDOM_FD;
   {
      char real[160];
      const char *sp = bionic_selfproc(path, real, sizeof(real));
      if (sp)
         return open(sp, O_RDONLY);
   }
   if (path && strncmp(path, "/proc/", 6) == 0 && !getenv("ROBLOX_NO_PROC")) {
      const int pfd = bionic_proc_open(path, "open");
      if (pfd < 0) {
         static unsigned said;
         if (said++ < 8)
            bionic_say("open(%s): no /proc answer", path);
         errno = ENOENT;
      }
      return pfd;
   }
   if (bionic_path_absent(path)) {
      bionic_say("open(%s): not on this system", path ? path : "NULL");
      errno = ENOENT;
      return -1;
   }
   const int fd = bionic_track_openat ? ps5_openat(AT_FDCWD, path, bionic_open_flags(linux_flags), mode)
                                      : open(path, bionic_open_flags(linux_flags), mode);
   bionic_say("open(%s, 0x%x) = %d", path, linux_flags, fd);
   return fd;
}

static int
bionic_open_2(const char *path, int linux_flags)
{
   BIONIC_PATH(path);
   return bionic_open(path, linux_flags & ~0x40);
}

/* Linux x86_64's struct stat, as Bionic has it (144 bytes). */
struct bionic_stat {
   uint64_t st_dev, st_ino, st_nlink;
   uint32_t st_mode, st_uid, st_gid, pad0;
   uint64_t st_rdev;
   int64_t st_size, st_blksize, st_blocks;
   int64_t st_atime_s, st_atime_ns, st_mtime_s, st_mtime_ns, st_ctime_s, st_ctime_ns;
   int64_t reserved[3];
};
_Static_assert(sizeof(struct bionic_stat) == 144, "Linux x86_64 struct stat is 144 bytes");

static void
bionic_fill_stat(struct bionic_stat *out, const struct stat *in)
{
   memset(out, 0, sizeof(*out));
   out->st_dev = (uint64_t)in->st_dev;
   out->st_ino = (uint64_t)in->st_ino;
   out->st_nlink = (uint64_t)in->st_nlink;
   out->st_mode = (uint32_t)in->st_mode; /* S_IF* and permission bits are the same */
   out->st_uid = (uint32_t)in->st_uid;
   out->st_gid = (uint32_t)in->st_gid;
   out->st_rdev = (uint64_t)in->st_rdev;
   out->st_size = (int64_t)in->st_size;
   out->st_blksize = (int64_t)in->st_blksize;
   out->st_blocks = (int64_t)in->st_blocks;
   out->st_atime_s = (int64_t)in->st_atim.tv_sec;
   out->st_atime_ns = (int64_t)in->st_atim.tv_nsec;
   out->st_mtime_s = (int64_t)in->st_mtim.tv_sec;
   out->st_mtime_ns = (int64_t)in->st_mtim.tv_nsec;
   out->st_ctime_s = (int64_t)in->st_ctim.tv_sec;
   out->st_ctime_ns = (int64_t)in->st_ctim.tv_nsec;
}

static int
bionic_stat_fn(const char *path, struct bionic_stat *out)
{
   BIONIC_PATH(path);
   if (bionic_path_absent(path)) {
      errno = ENOENT;
      return -1;
   }
   struct stat st;
   const int rc = stat(path, &st);
   if (rc == 0)
      bionic_fill_stat(out, &st);
   bionic_say("stat(%s) = %d%s", path, rc, rc ? " (missing)" : "");
   return rc;
}

static int
bionic_fstat_fn(int fd, struct bionic_stat *out)
{
   if ((fd >= BIONIC_PROC_BASE_FD && fd < BIONIC_PROC_BASE_FD + BIONIC_PROC_FDS) || fd == BIONIC_URANDOM_FD) {
      /* as Linux reports them: /proc files are size-0 regular files,
       * urandom a character device */
      memset(out, 0, sizeof(*out));
      out->st_mode = fd == BIONIC_URANDOM_FD ? 020666 : 0100444;
      out->st_nlink = 1;
      out->st_blksize = 1024;
      return 0;
   }
   struct stat st;
   const int rc = fstat(fd, &st);
   if (rc == 0)
      bionic_fill_stat(out, &st);
   return rc;
}

static ssize_t
bionic_readlink(const char *path, char *buf, size_t n)
{
   BIONIC_PATH(path);
   (void)buf;
   (void)n;
   bionic_say("readlink(%s): not provided", path ? path : "NULL");
   errno = ENOENT;
   return -1;
}

#include "bionic_file_lock.h"

static void bn_trace_fcntl(int fd, int cmd, long arg);
static int bn_is_socket(int fd);
static int bn_is_nonblock(int fd);
static int bn_set_nonblock(int fd, int on);
static int
bionic_fcntl(int fd, int cmd, ...)
{
   va_list ap;
   va_start(ap, cmd);
   long arg = 0;
   if (cmd == 5 || cmd == 6 || cmd == 7)
      arg = (long)va_arg(ap, struct bionic_file_lock *);
   else if (cmd == 2 || cmd == 4)
      arg = va_arg(ap, int);
   va_end(ap);
   bn_trace_fcntl(fd, cmd, arg);
   switch (cmd) {
   case 5:
   case 6:
   case 7: return bionic_file_lock(fd, cmd, (struct bionic_file_lock *)arg);
   case 1: return fcntl(fd, F_GETFD);
   case 2: return fcntl(fd, F_SETFD, (int)arg);
   case 3: {
      const int f = fcntl(fd, F_GETFL);
      if (f < 0)
         return f;
      return (f & 3) | ((f & O_NONBLOCK) || bn_is_nonblock(fd) ? 0x800 : 0) | ((f & O_APPEND) ? 0x400 : 0);
   }
   case 4:
      if (bn_is_socket(fd))
         return bn_set_nonblock(fd, (arg & 0x800) != 0);
      {
      /* only O_NONBLOCK / O_APPEND change with F_SETFL; passing the access
       * mode bits back (curl does flags|O_NONBLOCK from F_GETFL) fails on
       * PS5 sockets, and curl then gives up on the request
       * (CURLE_UNSUPPORTED_PROTOCOL, which Roblox shows as InvalidUrl) */
      const int cur = fcntl(fd, F_GETFL);
      int f = (cur < 0 ? 0 : cur) & ~(O_NONBLOCK | O_APPEND);
      if (arg & 0x800) f |= O_NONBLOCK;
      if (arg & 0x400) f |= O_APPEND;
      int r = fcntl(fd, F_SETFL, f);
      if (r != 0) {
         int on = (arg & 0x800) != 0;
         const int e = errno;
         r = ioctl(fd, FIONBIO, &on);
         static unsigned said;
         if (said++ < 4)
            bionic_say("fcntl(%d, F_SETFL, 0x%lx): PS5 refused (errno %d); FIONBIO -> %d", fd, arg, e, r);
      }
      return r == 0 ? 0 : -1;
   }
   default:
      bionic_say("fcntl(%d, %d): not mapped", fd, cmd);
      errno = EINVAL;
      return -1;
   }
}

/* fopen / fwrite: log which files the library opens, and echo what it writes
 * to the files it opened for writing (its own log files) into the report. */
static FILE *bionic_written[16];
/* The engine writes every log line to its own log file (files/appData/logs/
 * *_last.log) with fputs and then fflush, on the thread that logs: the game
 * thread too, while it holds the DataModel lock. On the PS5 such a flush to
 * /data sometimes blocks for 0.5-0.9 s, and every frame waits (seen in
 * freeze samples: the game thread in fflush via lib+0x1effab9). The same
 * lines reach the report through liblog, so that file gets a 256 KiB buffer
 * and its per-line fflush does nothing; it is written when the buffer fills
 * and at fclose. ROBLOX_FLUSH_LOG_FILE=1 keeps the per-line flush. */
static FILE *bionic_unflushed[8];

static int
bionic_fflush(FILE *f)
{
   if (f)
      for (int i = 0; i < 8; i++)
         if (__atomic_load_n(&bionic_unflushed[i], __ATOMIC_ACQUIRE) == f)
            return 0;
   return fflush(f);
}

static int
bionic_fclose(FILE *f)
{
   for (int i = 0; i < 8; i++)
      if (__atomic_load_n(&bionic_unflushed[i], __ATOMIC_ACQUIRE) == f)
         __atomic_store_n(&bionic_unflushed[i], (FILE *)NULL, __ATOMIC_RELEASE);
   for (int i = 0; i < 16; i++)
      if (bionic_written[i] == f)
         bionic_written[i] = NULL;
   return fclose(f);
}
static FILE *
bionic_fopen(const char *path, const char *mode)
{
   BIONIC_PATH(path);
   {
      char real[160];
      const char *sp = bionic_selfproc(path, real, sizeof(real));
      if (sp)
         return fopen(sp, "rb");
   }
   if (bionic_path_absent(path)) {
      bionic_say("fopen(%s, %s): not on this system", path ? path : "NULL", mode ? mode : "");
      errno = ENOENT;
      return NULL;
   }
   FILE *f = fopen(path, mode);
   bionic_say("fopen(%s, %s) = %s", path ? path : "NULL", mode ? mode : "", f ? "ok" : "failed");
   if (f && path && mode && (strchr(mode, 'w') || strchr(mode, 'a')) && strstr(path, "/files/appData/logs/") &&
       !getenv("ROBLOX_FLUSH_LOG_FILE")) {
      /* the engine's own log file: see bionic_fflush */
      setvbuf(f, NULL, _IOFBF, 256 << 10);
      for (int i = 0; i < 8; i++)
         if (!__atomic_load_n(&bionic_unflushed[i], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&bionic_unflushed[i], f, __ATOMIC_RELEASE);
            break;
         }
   }
   if (f && mode && (strchr(mode, 'w') || strchr(mode, 'a')))
      for (int i = 0; i < 16; i++)
         if (!bionic_written[i]) {
            bionic_written[i] = f;
            break;
         }
   return f;
}

static size_t
bionic_fwrite(const void *buf, size_t size, size_t count, FILE *f)
{
   bool echo = f == stdout || f == stderr;
   for (int i = 0; i < 16; i++)
      if (bionic_written[i] && bionic_written[i] == f)
         echo = true;
   if (echo) {
      {
         char text[360];
         size_t n = size * count < sizeof(text) - 1 ? size * count : sizeof(text) - 1;
         memcpy(text, buf, n);
         text[n] = 0;
         for (size_t k = 0; k < n; k++)
            if (text[k] == '\n' || text[k] == '\r')
               text[k] = ' ';
         bionic_say("file> %s", text);
      }
   }
   return fwrite(buf, size, count, f);
}

/* ---- ANativeWindow: one fixed 1920x1080 window (the TV) ---------------------- */
struct bionic_window {
   uint32_t magic;
   int width, height, refs;
};
static struct bionic_window bionic_the_window = {0x574e4457, 1920, 1080, 1};
static void *
bionic_ANativeWindow_fromSurface(void *env, void *surface)
{
   (void)env;
   bionic_say("ndk: ANativeWindow_fromSurface(%p) -> window %dx%d", surface, bionic_the_window.width,
              bionic_the_window.height);
   bionic_the_window.refs++;
   return &bionic_the_window;
}
static void bionic_ANativeWindow_acquire(struct bionic_window *w) { if (w) w->refs++; }
static void bionic_ANativeWindow_release(struct bionic_window *w) { if (w) w->refs--; }
static int bionic_ANativeWindow_getWidth(struct bionic_window *w) { return w ? w->width : 0; }
static int bionic_ANativeWindow_getHeight(struct bionic_window *w) { return w ? w->height : 0; }

static int bionic_access(const char *path, int mode) { BIONIC_PATH(path); return access(path, mode); }
static int bionic_remove(const char *path) { BIONIC_PATH(path); return remove(path); }
static int bionic_rmdir(const char *path) { BIONIC_PATH(path); return rmdir(path); }
static int bionic_unlink(const char *path) { BIONIC_PATH(path); return unlink(path); }
static int bionic_rename(const char *a, const char *b) { BIONIC_PATH(a); BIONIC_PATH(b); return rename(a, b); }

/* ---- system information ------------------------------------------------------ */

static char *
bionic_getcwd(char *buf, size_t size)
{
   static char own[64];
   if (!buf) {
      buf = own;
      size = sizeof(own);
   }
   /* The app's files directory (writable), like Android's cwd for the app's
    * own relative paths ("./appData/..."). */
   static const char cwd[] = "/data/roblox/files";
   if (size < sizeof(cwd)) {
      errno = ERANGE;
      return NULL;
   }
   strcpy(buf, cwd);
   return buf;
}

/* Directories: Bionic's struct dirent is Linux's dirent64. */
#include <dirent.h>
struct bionic_dirent {
   uint64_t d_ino;
   int64_t d_off;
   uint16_t d_reclen;
   uint8_t d_type;
   char d_name[256];
};

struct bionic_dir {
   DIR *dir;
   struct bionic_dirent entry;
};

static void *
bionic_opendir(const char *path)
{
   BIONIC_PATH(path);
   if (bionic_path_absent(path)) {
      errno = ENOENT;
      return NULL;
   }
   DIR *d = opendir(path);
   if (!d)
      return NULL;
   struct bionic_dir *b = calloc(1, sizeof(*b));
   b->dir = d;
   return b;
}

static struct bionic_dirent *
bionic_readdir(struct bionic_dir *b)
{
   struct dirent *e = b ? readdir(b->dir) : NULL;
   if (!e)
      return NULL;
   memset(&b->entry, 0, sizeof(b->entry));
   b->entry.d_ino = (uint64_t)e->d_fileno;
   b->entry.d_reclen = sizeof(b->entry);
   b->entry.d_type = e->d_type; /* DT_* values are the same */
   snprintf(b->entry.d_name, sizeof(b->entry.d_name), "%s", e->d_name);
   return &b->entry;
}

static int
bionic_closedir(struct bionic_dir *b)
{
   if (!b)
      return -1;
   const int rc = closedir(b->dir);
   free(b);
   return rc;
}

static int
bionic_mkdir(const char *path, int mode)
{
   BIONIC_PATH(path);
   const int rc = mkdir(path, (mode_t)mode);
   bionic_say("mkdir(%s) = %d", path ? path : "NULL", rc);
   return rc;
}

static char *
bionic_realpath(const char *path, char *out)
{
   BIONIC_PATH(path);
   static char own[1024];
   if (!out)
      out = own;
   snprintf(out, 1024, "%s", path ? path : "");
   return out;
}

struct bionic_sysinfo {
   long uptime;
   unsigned long loads[3];
   unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
   unsigned short procs, pad;
   unsigned long totalhigh, freehigh;
   unsigned int mem_unit;
   /* Linux's _f[20 - 2 * sizeof(long) - sizeof(int)] is empty on 64-bit. */
};
_Static_assert(sizeof(struct bionic_sysinfo) == 112, "Linux x86_64 struct sysinfo is 112 bytes");

static int
bionic_sysinfo_fn(struct bionic_sysinfo *si)
{
   memset(si, 0, sizeof(*si));
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   si->uptime = (long)ts.tv_sec;
   si->totalram = 4ul << 30; /* what a title can count on, roughly */
   si->freeram = 2ul << 30;
   si->procs = 1;
   si->mem_unit = 1;
   return 0;
}

struct bionic_utsname {
   char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

static int
bionic_uname(struct bionic_utsname *u)
{
   memset(u, 0, sizeof(*u));
   strcpy(u->sysname, "Linux");
   strcpy(u->nodename, "ps5");
   strcpy(u->release, "5.10.0-ps5");
   strcpy(u->version, "#1");
   strcpy(u->machine, "x86_64");
   strcpy(u->domainname, "(none)");
   return 0;
}

/* ---- multibyte, C locale (Bionic's mbstate_t is 4 bytes, the system's is
 * 128, so none of these go to the system) ------------------------------------ */

static size_t bionic_ctype_get_mb_cur_max(void) { return 1; }

static int
bionic_mbtowc(int *wc, const char *s, size_t n)
{
   if (!s)
      return 0;
   if (n == 0)
      return -1;
   if (wc)
      *wc = (unsigned char)*s;
   return *s ? 1 : 0;
}

static size_t
bionic_mbrtowc(int *wc, const char *s, size_t n, void *ps)
{
   (void)ps;
   if (!s)
      return 0;
   if (n == 0)
      return (size_t)-2;
   if (wc)
      *wc = (unsigned char)*s;
   return *s ? 1 : 0;
}

static size_t bionic_mbrlen(const char *s, size_t n, void *ps) { return bionic_mbrtowc(NULL, s, n, ps); }

static size_t
bionic_wcrtomb(char *s, int wc, void *ps)
{
   (void)ps;
   if (!s)
      return 1;
   if (wc < 0 || wc > 0xff) {
      errno = EILSEQ;
      return (size_t)-1;
   }
   *s = (char)wc;
   return 1;
}

static size_t
bionic_mbsnrtowcs(int *dst, const char **src, size_t nms, size_t len, void *ps)
{
   (void)ps;
   size_t i = 0;
   const char *s = *src;
   for (; i < nms && (!dst || i < len); i++) {
      if (dst)
         dst[i] = (unsigned char)s[i];
      if (!s[i]) {
         if (dst)
            *src = NULL;
         return i;
      }
   }
   if (dst)
      *src = s + i;
   return i;
}

static size_t
bionic_mbsrtowcs(int *dst, const char **src, size_t len, void *ps)
{
   return bionic_mbsnrtowcs(dst, src, (size_t)-1, len, ps);
}

static size_t
bionic_wcsnrtombs(char *dst, const int **src, size_t nwc, size_t len, void *ps)
{
   (void)ps;
   size_t i = 0;
   const int *s = *src;
   for (; i < nwc && (!dst || i < len); i++) {
      if (dst)
         dst[i] = (char)s[i];
      if (!s[i]) {
         if (dst)
            *src = NULL;
         return i;
      }
   }
   if (dst)
      *src = s + i;
   return i;
}

/* ---- the table ------------------------------------------------------------ */

/* ---- Android NDK: ALooper on poll(), AConfiguration, AAssetManager ---------
 * GameActivity's glue talks to its game thread through a pipe that the game
 * thread's ALooper watches; this looper polls the registered descriptors for
 * real, so the commands get through. */

#include <poll.h>

#define BIONIC_LOOPER_FDS 16
struct bionic_looper {
   int count, said_first;
   struct {
      int fd, ident, events;
      int (*cb)(int fd, int events, void *data);
      void *data;
   } fds[BIONIC_LOOPER_FDS];
};

static pthread_key_t bionic_looper_key;
static pthread_once_t bionic_looper_once = PTHREAD_ONCE_INIT;
static void bionic_looper_key_init(void) { pthread_key_create(&bionic_looper_key, NULL); }

static void *
bionic_ALooper_forThread(void)
{
   pthread_once(&bionic_looper_once, bionic_looper_key_init);
   return pthread_getspecific(bionic_looper_key);
}

static void *
bionic_ALooper_prepare(int opts)
{
   (void)opts;
   struct bionic_looper *l = bionic_ALooper_forThread();
   if (!l) {
      l = calloc(1, sizeof(*l));
      pthread_setspecific(bionic_looper_key, l);
      bionic_say("ndk: ALooper_prepare: new looper %p", (void *)l);
   }
   return l;
}

static void bionic_ALooper_acquire(void *l) { (void)l; }
static void bionic_ALooper_release(void *l) { (void)l; }

static int
bionic_ALooper_addFd(struct bionic_looper *l, int fd, int ident, int events,
                     int (*cb)(int, int, void *), void *data)
{
   if (!l)
      return -1;
   for (int i = 0; i < l->count; i++)
      if (l->fds[i].fd == fd) {
         l->fds[i].ident = ident, l->fds[i].events = events, l->fds[i].cb = cb, l->fds[i].data = data;
         return 1;
      }
   if (l->count >= BIONIC_LOOPER_FDS)
      return -1;
   l->fds[l->count].fd = fd;
   l->fds[l->count].ident = ident;
   l->fds[l->count].events = events;
   l->fds[l->count].cb = cb;
   l->fds[l->count].data = data;
   l->count++;
   bionic_say("ndk: ALooper_addFd(fd %d, ident %d)", fd, ident);
   return 1;
}

static int
bionic_ALooper_removeFd(struct bionic_looper *l, int fd)
{
   if (!l)
      return -1;
   for (int i = 0; i < l->count; i++)
      if (l->fds[i].fd == fd) {
         l->fds[i] = l->fds[--l->count];
         return 1;
      }
   return 0;
}

/* ALOOPER_POLL_TIMEOUT -3, ALOOPER_POLL_CALLBACK -2; ALOOPER_EVENT_INPUT 1. */
static int
bionic_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
   struct bionic_looper *l = bionic_ALooper_forThread();
   if (!l || l->count == 0) {
      usleep((useconds_t)((timeout_ms < 0 || timeout_ms > 16 ? 16 : timeout_ms) * 1000));
      return -3;
   }
   struct pollfd p[BIONIC_LOOPER_FDS];
   for (int i = 0; i < l->count; i++) {
      p[i].fd = l->fds[i].fd;
      p[i].events = POLLIN;
      p[i].revents = 0;
   }
   if (!l->said_first) {
      l->said_first = 1;
      bionic_say("ndk: ALooper_pollOnce: first poll on looper %p, %d fds, timeout %d", (void *)l, l->count, timeout_ms);
   }
   const int n = poll(p, (nfds_t)l->count, timeout_ms);
   if (n < 0) {
      static int warned;
      if (warned++ < 3)
         bionic_say("ndk: poll failed: errno %d", errno);
      usleep(16000);
   }
   if (n <= 0)
      return -3;
   {
      static unsigned fired;
      if (fired++ < 40)
         for (int i = 0; i < l->count; i++)
            if (p[i].revents)
               bionic_say("ndk: looper %p: fd %d ready (ident %d%s)", (void *)l, p[i].fd, l->fds[i].ident,
                          l->fds[i].cb ? ", callback" : "");
   }
   for (int i = 0; i < l->count; i++) {
      if (!p[i].revents)
         continue;
      const int events = (p[i].revents & POLLIN) ? 1 : 4; /* INPUT / ERROR */
      if (l->fds[i].cb) {
         if (l->fds[i].cb(l->fds[i].fd, events, l->fds[i].data) == 0)
            bionic_ALooper_removeFd(l, l->fds[i].fd);
         return -2;
      }
      if (out_fd) *out_fd = l->fds[i].fd;
      if (out_events) *out_events = events;
      if (out_data) *out_data = l->fds[i].data;
      return l->fds[i].ident;
   }
   return -3;
}

/* One configuration: an English, 1080p landscape TV. */
static void *bionic_AConfiguration_new(void) { return calloc(1, 64); }
static void bionic_AConfiguration_delete(void *c) { free(c); }
static void bionic_AConfiguration_fromAssetManager(void *c, void *am) { (void)c; (void)am; }
static void bionic_AConfiguration_getLanguage(void *c, char *out) { (void)c; out[0] = 'e'; out[1] = 'n'; }
static void bionic_AConfiguration_getCountry(void *c, char *out) { (void)c; out[0] = 'U'; out[1] = 'S'; }
static int32_t bionic_AConfiguration_getScreenWidthDp(void *c) { (void)c; return 960; }
static int32_t bionic_AConfiguration_getScreenHeightDp(void *c) { (void)c; return 540; }
static int32_t bionic_AConfiguration_getScreenSize(void *c) { (void)c; return 4; /* XLARGE */ }
static int32_t bionic_AConfiguration_getNavHidden(void *c) { (void)c; return 0; }

/* Assets: the APK's assets/ folder, copied beside the title. */
static int bionic_asset_manager;
static void *bionic_AAssetManager_fromJava(void *env, void *obj) { (void)env; (void)obj; return &bionic_asset_manager; }

struct bionic_asset {
   uint8_t *data;
   off_t length;
   char path[512];
};

static void *
bionic_AAssetManager_open(void *mgr, const char *name, int mode)
{
   (void)mgr;
   (void)mode;
   char path[512];
   snprintf(path, sizeof(path), "/app0/assets/%s", name ? name : "");
   FILE *f = fopen(path, "rb");
   if (!f) {
      bionic_say("ndk: AAssetManager_open(%s): not found", name ? name : "NULL");
      return NULL;
   }
   fseek(f, 0, SEEK_END);
   const long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   struct bionic_asset *a = calloc(1, sizeof(*a));
   snprintf(a->path, sizeof(a->path), "%s", path);
   a->data = malloc(n > 0 ? (size_t)n : 1);
   a->length = (off_t)(fread(a->data, 1, (size_t)(n > 0 ? n : 0), f));
   fclose(f);
   bionic_say("ndk: AAssetManager_open(%s): %ld bytes", name, (long)a->length);
   return a;
}

static const void *bionic_AAsset_getBuffer(struct bionic_asset *a) { return a ? a->data : NULL; }
static off_t bionic_AAsset_getLength(struct bionic_asset *a) { return a ? a->length : 0; }
/* A descriptor of the asset's own file, with the asset at [start, start+length).
 * (Unprovided it returned 0: the engine wrapped stdin in a FILE and its render
 * thread waited on it for ever while loading the shader pack.) */
static int
bionic_AAsset_openFileDescriptor(struct bionic_asset *a, off_t *start, off_t *length)
{
   if (!a || !a->path[0])
      return -1;
   const int fd = open(a->path, O_RDONLY);
   bionic_say("ndk: AAsset_openFileDescriptor(%s) = %d, %ld bytes", a->path, fd, (long)a->length);
   if (fd < 0)
      return -1;
   if (start)
      *start = 0;
   if (length)
      *length = a->length;
   return fd;
}

static void
bionic_AAsset_close(struct bionic_asset *a)
{
   if (a) {
      free(a->data);
      free(a);
   }
}

static int
bionic_pipe(int fds[2])
{
   return pipe(fds);
}

#include "bionic_net.h"

/* ---- the table (continued below) ------------------------------------------ */

#define BIONIC_DIRECT_ENTRY(name) {#name, (void *)bionic_direct_##name},


/* ---- imports that used to fall through to the trap (which returns 0) -------
 * A 0 from a function that makes a descriptor is "fd 0", from mremap it is
 * "address 0", from fork it is "you are the child": all worse than a clean
 * failure. Errors are given in Linux's numbering, which is what the engine
 * compares against (the PS5's errno values differ, e.g. EAGAIN 35 vs 11). */
#include <wchar.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <utime.h>

enum { BL_EPERM = 1, BL_EINTR = 4, BL_EAGAIN = 11, BL_ENOMEM = 12, BL_EACCES = 13, BL_EINVAL = 22,
       BL_ENOTTY = 25, BL_ENOSYS = 38 };


/* statvfs: Android x86_64 layout (unsigned long f_bsize, f_frsize; u64
 * f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail; unsigned long
 * f_fsid, f_flag, f_namemax; u32 spare[6]) = 112 bytes. A title cannot
 * measure the data partition (statfs is not available to it), so the
 * answer is the PS5 platform library's: a writable filesystem with 16 GiB
 * free, the same as ps5_statvfs gives. The engine's storage manager asks
 * this before it keeps downloaded content; ENOSYS made it log "Failed to get
 * available disk space" for its cache. */
struct bionic_statvfs {
   uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flag, namemax;
   uint32_t spare[6];
};
static int
bionic_statvfs_fn(const char *path, struct bionic_statvfs *out)
{
   BIONIC_PATH(path);
   struct stat st;
   if (!out || bionic_path_absent(path) || stat(path, &st) != 0) {
      errno = ENOENT;
      return -1;
   }
   memset(out, 0, sizeof(*out));
   out->bsize = out->frsize = 4096;
   out->blocks = (64ull << 30) / 4096;
   out->bfree = out->bavail = (16ull << 30) / 4096;
   out->files = 1u << 20;
   out->ffree = out->favail = 1u << 19;
   out->namemax = 255;
   static int said;
   if (said++ < 5)
      bionic_say("statvfs(%s): 16 GiB free of 64 GiB (assumed; the PS5 gives a title no measure)", path);
   return 0;
}

static int bionic_fail_enosys(void) { errno = BL_ENOSYS; return -1; }
static int bionic_fail_enotty(void) { errno = BL_ENOTTY; return -1; }
static int bionic_ok_zero(void) { return 0; }

/* eventfd: a pipe. The engine's fd is the read end (so poll() on it works);
 * writes to it go to the paired write end. Each write is one 8-byte value;
 * a read sums what is queued (or takes 1 in semaphore mode). */
static pthread_mutex_t bionic_efd_lock = PTHREAD_MUTEX_INITIALIZER;

static int
bionic_efd_find(int fd)
{
   for (int i = 0; i < BIONIC_EFD_MAX; i++)
      if (bionic_efd[i].rfd == fd && bionic_efd[i].wfd > 0)
         return i;
   return -1;
}

static int
bionic_eventfd(unsigned initval, int flags)
{
   int p[2];
   if (pipe(p) != 0) {
      errno = BL_ENOMEM;
      return -1;
   }
   fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
   if (flags & 04000) /* Linux EFD_NONBLOCK == O_NONBLOCK 04000 */
      fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
   pthread_mutex_lock(&bionic_efd_lock);
   int slot = -1;
   for (int i = 0; i < BIONIC_EFD_MAX; i++)
      if (bionic_efd[i].wfd <= 0) {
         slot = i;
         break;
      }
   if (slot >= 0) {
      bionic_efd[slot].rfd = p[0];
      bionic_efd[slot].wfd = p[1];
      bionic_efd[slot].sem = flags & 1;
   }
   pthread_mutex_unlock(&bionic_efd_lock);
   if (slot < 0) {
      close(p[0]);
      close(p[1]);
      errno = BL_ENOMEM;
      return -1;
   }
   if (initval) {
      uint64_t v = initval;
      (void)!write(p[1], &v, 8);
   }
   static unsigned said;
   if (said++ < 8)
      bionic_say("eventfd(%u, 0x%x) = %d (pipe-backed)", initval, flags, p[0]);
   return p[0];
}

static ssize_t
bionic_efd_read(int slot, void *buf, size_t n)
{
   if (n < 8) {
      errno = BL_EINVAL;
      return -1;
   }
   const int fd = bionic_efd[slot].rfd;
   uint64_t v = 0, sum = 0;
   ssize_t got = read(fd, &v, 8); /* blocks unless EFD_NONBLOCK */
   if (got != 8) {
      errno = (got < 0 && errno == EAGAIN) ? BL_EAGAIN : (got < 0 && errno == EINTR) ? BL_EINTR : BL_EAGAIN;
      return -1;
   }
   if (bionic_efd[slot].sem) {
      if (v > 1) {
         uint64_t rest = v - 1;
         (void)!write(bionic_efd[slot].wfd, &rest, 8);
      }
      sum = 1;
   } else {
      sum = v;
      const int fl = fcntl(fd, F_GETFL);
      fcntl(fd, F_SETFL, fl | O_NONBLOCK);
      while (read(fd, &v, 8) == 8)
         sum += v;
      fcntl(fd, F_SETFL, fl);
   }
   memcpy(buf, &sum, 8);
   return 8;
}

static ssize_t
bionic_write(int fd, const void *buf, size_t n)
{
   if (fd >= 0x7000) { /* the synthetic fds are read-only */
      errno = BL_EINVAL;
      return -1;
   }
   pthread_mutex_lock(&bionic_efd_lock);
   const int slot = bionic_efd_find(fd);
   const int wfd = slot >= 0 ? bionic_efd[slot].wfd : -1;
   pthread_mutex_unlock(&bionic_efd_lock);
   if (slot >= 0) {
      if (n < 8) {
         errno = BL_EINVAL;
         return -1;
      }
      if (write(wfd, buf, 8) != 8) {
         errno = BL_EAGAIN;
         return -1;
      }
      return 8;
   }
   return write(fd, buf, n);
}

/* by value: Bionic LP64 struct mallinfo is 10 size_t (returned through a
 * hidden pointer the trap stub would never fill in) */
struct bionic_mallinfo { size_t f[10]; };
static struct bionic_mallinfo bionic_mallinfo_fn(void) { struct bionic_mallinfo m; memset(&m, 0, sizeof(m)); return m; }

static void *
bionic_mremap(void *old, size_t olds, size_t news, int flags, ...)
{
   (void)old; (void)olds; (void)news; (void)flags;
   errno = BL_ENOMEM; /* callers fall back to mmap + copy */
   return (void *)-1;
}

static int bionic_raise(int sig) { bionic_say("raise(linux %d) ignored", sig); return 0; }
static int bionic_sched_getparam(int pid, int *param) { (void)pid; if (param) *param = 0; return 0; }
static int bionic_posix_fallocate(int fd, long off, long len) { (void)fd; (void)off; (void)len; return 0; }

/* Bionic LP64 struct lconv: 10 char* then 14 char, as POSIX orders them. */
static struct {
   const char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
      *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
   char c[14];
} bionic_lconv = {".", "", "", "", "", "", "", "", "", "", {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}};
static void *bionic_localeconv(void) { return &bionic_lconv; }

/* C-locale wide/collation helpers: the locale argument is the stand-in one. */
static int bionic_iswalpha_l(wint_t c, void *l) { (void)l; return (c < 128 && ((c | 32) - 97u) < 26u); }
static int bionic_iswblank_l(wint_t c, void *l) { (void)l; return (c == 32 || c == 9); }
static int bionic_iswcntrl_l(wint_t c, void *l) { (void)l; return (c < 32 || c == 127); }
static int bionic_iswdigit_l(wint_t c, void *l) { (void)l; return ((c - 48u) < 10u); }
static int bionic_iswlower_l(wint_t c, void *l) { (void)l; return ((c - 97u) < 26u); }
static int bionic_iswprint_l(wint_t c, void *l) { (void)l; return (c >= 32 && c < 127); }
static int bionic_iswpunct_l(wint_t c, void *l) { (void)l; return (c > 32 && c < 127 && !(((c | 32) - 97u) < 26u) && !((c - 48u) < 10u)); }
static int bionic_iswspace_l(wint_t c, void *l) { (void)l; return (c == 32 || (c - 9u) < 5u); }
static int bionic_iswupper_l(wint_t c, void *l) { (void)l; return ((c - 65u) < 26u); }
static int bionic_iswxdigit_l(wint_t c, void *l) { (void)l; return (((c - 48u) < 10u) || (((c | 32) - 97u) < 6u)); }
static wint_t bionic_towlower_l(wint_t c, void *l) { (void)l; return (c - 65u) < 26u ? c + 32 : c; }
static wint_t bionic_towupper_l(wint_t c, void *l) { (void)l; return (c - 97u) < 26u ? c - 32 : c; }
static int bionic_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static size_t bionic_strxfrm_l(char *d, const char *s, size_t n, void *l)
{
   (void)l;
   const size_t len = strlen(s);
   if (n) {
      const size_t k = len < n - 1 ? len : n - 1;
      memcpy(d, s, k);
      d[k] = 0;
   }
   return len;
}
static int bionic_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscmp(a, b); }
static size_t bionic_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l)
{
   (void)l;
   const size_t len = wcslen(s);
   if (n) {
      const size_t k = len < n - 1 ? len : n - 1;
      wmemcpy(d, s, k);
      d[k] = 0;
   }
   return len;
}


/* pread / lseek on the synthetic descriptors (/proc files, urandom): the
 * engine re-reads /proc/meminfo with pread, which the system cannot serve
 * for a descriptor that exists only here. */
static int bionic_is_proc_fd(int fd) { return fd >= BIONIC_PROC_BASE_FD && fd < BIONIC_PROC_BASE_FD + BIONIC_PROC_FDS; }
static ssize_t
bionic_pread(int fd, void *buf, size_t n, long off)
{
   if (fd == BIONIC_URANDOM_FD) {
      bionic_getentropy(buf, n);
      return (ssize_t)n;
   }
   if (bionic_is_proc_fd(fd)) {
      const int i = fd - BIONIC_PROC_BASE_FD;
      if (off < 0) {
         errno = 22;
         return -1;
      }
      if ((size_t)off >= bionic_proc[i].len)
         return 0;
      const size_t left = bionic_proc[i].len - (size_t)off;
      const size_t take = n < left ? n : left;
      memcpy(buf, bionic_proc[i].data + off, take);
      return (ssize_t)take;
   }
   return pread(fd, buf, n, off);
}
static long
bionic_lseek(int fd, long off, int whence)
{
   if (bionic_is_proc_fd(fd)) {
      const int i = fd - BIONIC_PROC_BASE_FD;
      long base = whence == 0 ? 0 : whence == 1 ? (long)bionic_proc[i].pos : (long)bionic_proc[i].len;
      if (base + off < 0) {
         errno = 22;
         return -1;
      }
      bionic_proc[i].pos = (size_t)(base + off);
      return base + off;
   }
   if (fd == BIONIC_URANDOM_FD)
      return 0;
   return lseek(fd, off, whence);
}


/* Android's wchar_t is 4 bytes; the PS5 SDK's is 2 (unsigned short), so its
 * libc's wide-string functions walk 2-byte units. The engine uses wmemchr to
 * find an int in a std::vector<int> before erasing it: with 2-byte units the
 * search misses, and the erase moved -4 bytes (crash on teleport/leave).
 * These are Bionic's semantics on 32-bit units. */
typedef int32_t bionic_wchar;
static bionic_wchar *
bionic_wmemchr(const bionic_wchar *s, bionic_wchar c, size_t n)
{
   for (size_t i = 0; i < n; i++)
      if (s[i] == c)
         return (bionic_wchar *)(s + i);
   return NULL;
}
static int
bionic_wmemcmp(const bionic_wchar *a, const bionic_wchar *b, size_t n)
{
   for (size_t i = 0; i < n; i++)
      if (a[i] != b[i])
         return a[i] < b[i] ? -1 : 1;
   return 0;
}
static size_t
bionic_wcslen(const bionic_wchar *s)
{
   size_t n = 0;
   while (s[n])
      n++;
   return n;
}
static int
bionic_wcscoll(const bionic_wchar *a, const bionic_wchar *b)
{
   /* Bionic collates in the C locale: plain code-point order */
   for (;; a++, b++) {
      if (*a != *b)
         return *a < *b ? -1 : 1;
      if (!*a)
         return 0;
   }
}
static size_t
bionic_wcsxfrm(bionic_wchar *dst, const bionic_wchar *src, size_t n)
{
   const size_t len = bionic_wcslen(src);
   if (n) {
      const size_t c = len < n - 1 ? len : n - 1;
      memcpy(dst, src, c * sizeof(*src));
      dst[c] = 0;
   }
   return len;
}

static const struct ael_import bionic_imports[] = {
   BIONIC_DIRECT_LIST(BIONIC_DIRECT_ENTRY)
   {"__android_log_print", (void *)bionic_log_print},
   {"__android_log_write", (void *)bionic_log_write},
   {"__android_log_buf_write", (void *)bionic_log_buf_write},
   {"__android_log_assert", (void *)bionic_log_assert},
   {"android_set_abort_message", (void *)bionic_set_abort_message},
   {"__assert", (void *)bionic_assert_fail},
   {"__stack_chk_fail", (void *)bionic_stack_chk_fail},
   {"__stack_chk_guard", (void *)&bionic_stack_chk_guard},
   {"__errno", (void *)bionic_errno},
   {"stdin", (void *)&bionic_stdin},
   {"stdout", (void *)&bionic_stdout},
   {"stderr", (void *)&bionic_stderr},
   {"__sF", (void *)bionic_sF},
   {"environ", (void *)&bionic_environ},
   {"optarg", (void *)&bionic_optarg},
   {"optind", (void *)&bionic_optind},
   {"daylight", (void *)&bionic_daylight},
   {"timezone", (void *)&bionic_timezone},
   {"tzname", (void *)bionic_tzname},
   {"in6addr_any", (void *)bionic_in6addr_any},
   {"in6addr_loopback", (void *)bionic_in6addr_loopback},
   {"__cxa_atexit", (void *)bionic_cxa_atexit},
   {"__cxa_finalize", (void *)bionic_cxa_finalize},
   {"__cxa_thread_atexit_impl", (void *)bionic_cxa_thread_atexit_impl},
   {"__register_atfork", (void *)bionic_register_atfork},
   {"pthread_mutexattr_init", (void *)bionic_mutexattr_init},
   {"pthread_mutexattr_destroy", (void *)bionic_mutexattr_destroy},
   {"pthread_mutexattr_settype", (void *)bionic_mutexattr_settype},
   {"pthread_mutex_init", (void *)bionic_mutex_init},
   {"pthread_mutex_destroy", (void *)bionic_mutex_destroy},
   {"pthread_mutex_lock", (void *)bionic_mutex_lock},
   {"pthread_mutex_unlock", (void *)bionic_mutex_unlock},
   {"pthread_mutex_trylock", (void *)bionic_mutex_trylock},
   {"pthread_condattr_init", (void *)bionic_condattr_init},
   {"pthread_condattr_destroy", (void *)bionic_condattr_destroy},
   {"pthread_condattr_setclock", (void *)bionic_condattr_setclock},
   {"pthread_cond_init", (void *)bionic_cond_init},
   {"pthread_cond_destroy", (void *)bionic_cond_destroy},
   {"pthread_cond_signal", (void *)bionic_cond_signal},
   {"pthread_cond_broadcast", (void *)bionic_cond_broadcast},
   {"pthread_cond_wait", (void *)bionic_cond_wait},
   {"pthread_cond_timedwait", (void *)bionic_cond_timedwait},
   {"pthread_rwlock_init", (void *)bionic_rwlock_init},
   {"pthread_rwlock_destroy", (void *)bionic_rwlock_destroy},
   {"pthread_rwlock_rdlock", (void *)bionic_rwlock_rdlock},
   {"pthread_rwlock_wrlock", (void *)bionic_rwlock_wrlock},
   {"pthread_rwlock_unlock", (void *)bionic_rwlock_unlock},
   {"pthread_once", (void *)bionic_once},
   {"pthread_attr_init", (void *)bionic_attr_init},
   {"pthread_attr_destroy", (void *)bionic_attr_destroy},
   {"pthread_attr_setstacksize", (void *)bionic_attr_setstacksize},
   {"pthread_attr_setdetachstate", (void *)bionic_attr_setdetachstate},
   {"pthread_attr_setschedparam", (void *)bionic_attr_setschedparam},
   {"pthread_attr_getstack", (void *)bionic_attr_getstack},
   {"pthread_getattr_np", (void *)bionic_getattr_np},
   {"pthread_create", (void *)bionic_pthread_create},
   {"pthread_setname_np", (void *)bionic_setname_np},
   {"pthread_getschedparam", (void *)bionic_getschedparam},
   {"pthread_setschedparam", (void *)bionic_setschedparam},
   {"newlocale", (void *)bionic_newlocale},
   {"uselocale", (void *)bionic_uselocale},
   {"freelocale", (void *)bionic_freelocale},
   {"strtoull_l", (void *)bionic_strtoull_l},
   {"strtoll_l", (void *)bionic_strtoll_l},
   {"strtoul_l", (void *)bionic_strtoul_l},
   {"strtol_l", (void *)bionic_strtol_l},
   {"strtod_l", (void *)bionic_strtod_l},
   {"strtof_l", (void *)bionic_strtof_l},
   {"strtold_l", (void *)bionic_strtold_l},
   {"clock_gettime", (void *)bionic_clock_gettime},
   {"clock", (void *)bionic_clock_fn},
   {"mmap", (void *)bionic_mmap},
   {"munmap", (void *)bionic_munmap},
   {"mprotect", (void *)bionic_mprotect},
   {"madvise", (void *)bionic_madvise},
   {"sem_init", (void *)bionic_sem_init},
   {"sem_destroy", (void *)bionic_sem_destroy},
   {"sem_wait", (void *)bionic_sem_wait},
   {"sem_trywait", (void *)bionic_sem_trywait},
   {"sem_post", (void *)bionic_sem_post},
   {"sem_getvalue", (void *)bionic_sem_getvalue},
   {"sigemptyset", (void *)bionic_sigemptyset},
   {"sigfillset", (void *)bionic_sigfillset},
   {"sigaction", (void *)bionic_sigaction},
   {"pthread_sigmask", (void *)bionic_pthread_sigmask},
   {"sigaltstack", (void *)bionic_sigaltstack},
   {"signal", (void *)bionic_signal},
   {"strftime_l", (void *)bionic_strftime_l},
   {"sched_get_priority_max", (void *)bionic_sched_get_priority_max},
   {"sched_get_priority_min", (void *)bionic_sched_get_priority_min},
   {"sched_setscheduler", (void *)bionic_sched_setscheduler},
   {"sched_getscheduler", (void *)bionic_sched_getscheduler},
   {"sysconf", (void *)bionic_sysconf},
   {"getpagesize", (void *)bionic_getpagesize},
   {"getauxval", (void *)bionic_getauxval},
   {"gettid", (void *)bionic_gettid},
   {"prctl", (void *)bionic_prctl},
   {"syscall", (void *)bionic_syscall},
   {"sched_getcpu", (void *)bionic_sched_getcpu},
   {"__system_property_get", (void *)bionic_system_property_get},
   {"getentropy", (void *)bionic_getentropy},
   {"arc4random_buf", (void *)bionic_arc4random_buf},
   {"__memcpy_chk", (void *)bionic_direct_memcpy},
   {"__memmove_chk", (void *)bionic_direct_memmove},
   {"__memset_chk", (void *)bionic_direct_memset},
   {"__strlen_chk", (void *)bionic_direct_strlen},
   {"__strcpy_chk", (void *)bionic_direct_strcpy},
   {"__strcat_chk", (void *)bionic_direct_strcat},
   {"__strncpy_chk", (void *)bionic_direct_strncpy},
   {"__strchr_chk", (void *)bionic_direct_strchr},
   {"__strncpy_chk2", (void *)bionic_strncpy_chk2},
   {"__fread_chk", (void *)bionic_fread_chk},
   {"__fwrite_chk", (void *)bionic_fwrite_chk},
   {"__vsnprintf_chk", (void *)bionic_vsnprintf_chk},
   {"__vsprintf_chk", (void *)bionic_vsprintf_chk},
   {"__read_chk", (void *)bionic_read_chk2},
   {"__write_chk", (void *)bionic_write_chk},
   {"__readlink_chk", (void *)bionic_readlink_chk},
   {"__gnu_strerror_r", (void *)bionic_gnu_strerror_r},
   {"dl_iterate_phdr", (void *)bionic_dl_iterate_phdr},
   {"dlopen", (void *)bionic_dlopen},
   {"ANativeWindow_fromSurface", (void *)bionic_ANativeWindow_fromSurface},
   {"ANativeWindow_acquire", (void *)bionic_ANativeWindow_acquire},
   {"ANativeWindow_release", (void *)bionic_ANativeWindow_release},
   {"ANativeWindow_getWidth", (void *)bionic_ANativeWindow_getWidth},
   {"ANativeWindow_getHeight", (void *)bionic_ANativeWindow_getHeight},
   {"dlerror", (void *)bionic_dlerror},
   {"dlsym", (void *)bionic_dlsym},
   {"dlclose", (void *)bionic_dlclose},
   {"dladdr", (void *)bionic_dladdr},
   {"open", (void *)bionic_open},
   {"__open_2", (void *)bionic_open_2},
   {"stat", (void *)bionic_stat_fn},
   {"lstat", (void *)bionic_stat_fn},
   {"fstat", (void *)bionic_fstat_fn},
   {"readlink", (void *)bionic_readlink},
   {"fcntl", (void *)bionic_fcntl},
   {"sysinfo", (void *)bionic_sysinfo_fn},
   {"uname", (void *)bionic_uname},
   {"__ctype_get_mb_cur_max", (void *)bionic_ctype_get_mb_cur_max},
   {"wmemchr", (void *)bionic_wmemchr},
   {"wmemcmp", (void *)bionic_wmemcmp},
   {"wcslen", (void *)bionic_wcslen},
   {"wcscoll", (void *)bionic_wcscoll},
   {"wcsxfrm", (void *)bionic_wcsxfrm},
   {"mbtowc", (void *)bionic_mbtowc},
   {"mbrtowc", (void *)bionic_mbrtowc},
   {"mbrlen", (void *)bionic_mbrlen},
   {"wcrtomb", (void *)bionic_wcrtomb},
   {"mbsrtowcs", (void *)bionic_mbsrtowcs},
   {"mbsnrtowcs", (void *)bionic_mbsnrtowcs},
   {"wcsnrtombs", (void *)bionic_wcsnrtombs},
   {"read", (void *)bionic_read},
   {"close", (void *)bionic_close},
   {"strerror_r", (void *)bionic_strerror_r},
   {"openlog", (void *)bionic_openlog},
   {"syslog", (void *)bionic_syslog},
   {"closelog", (void *)bionic_closelog},
   {"ALooper_forThread", (void *)bionic_ALooper_forThread},
   {"ALooper_prepare", (void *)bionic_ALooper_prepare},
   {"ALooper_acquire", (void *)bionic_ALooper_acquire},
   {"ALooper_release", (void *)bionic_ALooper_release},
   {"ALooper_addFd", (void *)bionic_ALooper_addFd},
   {"ALooper_removeFd", (void *)bionic_ALooper_removeFd},
   {"ALooper_pollOnce", (void *)bionic_ALooper_pollOnce},
   {"AConfiguration_new", (void *)bionic_AConfiguration_new},
   {"AConfiguration_delete", (void *)bionic_AConfiguration_delete},
   {"AConfiguration_fromAssetManager", (void *)bionic_AConfiguration_fromAssetManager},
   {"AConfiguration_getLanguage", (void *)bionic_AConfiguration_getLanguage},
   {"AConfiguration_getCountry", (void *)bionic_AConfiguration_getCountry},
   {"AConfiguration_getScreenWidthDp", (void *)bionic_AConfiguration_getScreenWidthDp},
   {"AConfiguration_getScreenHeightDp", (void *)bionic_AConfiguration_getScreenHeightDp},
   {"AConfiguration_getScreenSize", (void *)bionic_AConfiguration_getScreenSize},
   {"AConfiguration_getNavHidden", (void *)bionic_AConfiguration_getNavHidden},
   {"AAssetManager_fromJava", (void *)bionic_AAssetManager_fromJava},
   {"AAssetManager_open", (void *)bionic_AAssetManager_open},
   {"AAsset_getBuffer", (void *)bionic_AAsset_getBuffer},
   {"AAsset_getLength", (void *)bionic_AAsset_getLength},
   {"AAsset_close", (void *)bionic_AAsset_close},
   {"AAsset_openFileDescriptor", (void *)bionic_AAsset_openFileDescriptor},
   {"AAsset_openFileDescriptor64", (void *)bionic_AAsset_openFileDescriptor},
   {"pipe", (void *)bionic_pipe},
   {"poll", (void *)bn_poll},
   {"exit", (void *)bionic_exit},
   {"getcwd", (void *)bionic_getcwd},
   {"access", (void *)bionic_access},
   {"remove", (void *)bionic_remove},
   {"rename", (void *)bionic_rename},
   {"rmdir", (void *)bionic_rmdir},
   {"unlink", (void *)bionic_unlink},
   {"fopen", (void *)bionic_fopen},
   {"fwrite", (void *)bionic_fwrite},
   {"fflush", (void *)bionic_fflush},
   {"fclose", (void *)bionic_fclose},
   {"opendir", (void *)bionic_opendir},
   {"readdir", (void *)bionic_readdir},
   {"closedir", (void *)bionic_closedir},
   {"mkdir", (void *)bionic_mkdir},
   {"realpath", (void *)bionic_realpath},
   {"_exit", (void *)bionic__exit},
   {"_Exit", (void *)bionic__exit},
   {"abort", (void *)bionic_abort},
   {"pthread_exit", (void *)bionic_pthread_exit},
   {"write", (void *)bionic_write},
   {"pread", (void *)bionic_pread},
   {"pread64", (void *)bionic_pread},
   {"lseek", (void *)bionic_lseek},
   {"lseek64", (void *)bionic_lseek},
   {"pwrite", (void *)pwrite},
   {"writev", (void *)writev},
   {"fchmod", (void *)fchmod},
   {"utimes", (void *)utimes},
   {"utime", (void *)utime},
   {"getppid", (void *)getppid},
   {"gethostname", (void *)gethostname},
   {"tzset", (void *)tzset},
   {"eventfd", (void *)bionic_eventfd},
   {"socket", (void *)bn_socket},
   {"recvmmsg", (void *)bn_recvmmsg},
   {"sendmmsg", (void *)bn_sendmmsg},
   {"sendto", (void *)bn_sendto},
   {"__sendto_chk", (void *)bn_sendto_chk},
   {"recvfrom", (void *)bn_recvfrom},
   {"sendmsg", (void *)bn_sendmsg},
   {"recvmsg", (void *)bn_recvmsg},
   {"setsockopt", (void *)bn_setsockopt},
   {"getsockopt", (void *)bn_getsockopt},
   {"getsockname", (void *)bn_getsockname},
   {"getpeername", (void *)bn_getpeername},
   {"shutdown", (void *)bn_shutdown},
   {"inet_pton", (void *)bn_inet_pton},
   {"inet_ntop", (void *)bn_inet_ntop},
   {"getnameinfo", (void *)bn_getnameinfo},
   {"socketpair", (void *)bionic_fail_enosys},
   {"epoll_create", (void *)bn_epoll_create},
   {"epoll_create1", (void *)bn_epoll_create1},
   {"epoll_ctl", (void *)bn_epoll_ctl},
   {"epoll_wait", (void *)bn_epoll_wait},
   {"timerfd_create", (void *)bionic_fail_enosys},
   {"timerfd_settime", (void *)bionic_fail_enosys},
   {"accept", (void *)bionic_fail_enosys},
   {"accept4", (void *)bionic_fail_enosys},
   {"bind", (void *)bn_bind},
   {"connect", (void *)bn_connect},
   {"listen", (void *)bionic_fail_enosys},
   {"select", (void *)bn_select},
   {"fork", (void *)bionic_fail_enosys},
   {"execv", (void *)bionic_fail_enosys},
   {"execve", (void *)bionic_fail_enosys},
   {"waitpid", (void *)bionic_fail_enosys},
   {"system", (void *)bionic_fail_enosys},
   {"ptrace", (void *)bionic_fail_enosys},
   {"statvfs", (void *)bionic_statvfs_fn},
   {"ioctl", (void *)bn_ioctl},
   {"tcgetattr", (void *)bionic_fail_enotty},
   {"tcsetattr", (void *)bionic_fail_enotty},
   {"msync", (void *)bionic_msync},
   {"mlock", (void *)bionic_ok_zero},
   {"setpriority", (void *)bionic_ok_zero},
   {"getpriority", (void *)bionic_ok_zero},
   {"mallinfo", (void *)bionic_mallinfo_fn},
   {"mremap", (void *)bionic_mremap},
   {"raise", (void *)bionic_raise},
   {"sched_getparam", (void *)bionic_sched_getparam},
   {"posix_fallocate", (void *)bionic_posix_fallocate},
   {"getaddrinfo", (void *)bn_getaddrinfo},
   {"freeaddrinfo", (void *)bn_freeaddrinfo},
   {"gai_strerror", (void *)bn_gai_strerror},
   {"localeconv", (void *)bionic_localeconv},
   {"iswalpha_l", (void *)bionic_iswalpha_l},
   {"iswblank_l", (void *)bionic_iswblank_l},
   {"iswcntrl_l", (void *)bionic_iswcntrl_l},
   {"iswdigit_l", (void *)bionic_iswdigit_l},
   {"iswlower_l", (void *)bionic_iswlower_l},
   {"iswprint_l", (void *)bionic_iswprint_l},
   {"iswpunct_l", (void *)bionic_iswpunct_l},
   {"iswspace_l", (void *)bionic_iswspace_l},
   {"iswupper_l", (void *)bionic_iswupper_l},
   {"iswxdigit_l", (void *)bionic_iswxdigit_l},
   {"towlower_l", (void *)bionic_towlower_l},
   {"towupper_l", (void *)bionic_towupper_l},
   {"strcoll_l", (void *)bionic_strcoll_l},
   {"strxfrm_l", (void *)bionic_strxfrm_l},
   {"wcscoll_l", (void *)bionic_wcscoll_l},
   {"wcsxfrm_l", (void *)bionic_wcsxfrm_l},
   {NULL, NULL},
};

static void
bionic_shims_init(void (*report_fn)(const char *))
{
   bionic_report = report_fn;
   bionic_table = bionic_imports;
   bionic_stdin = stdin;
   bionic_stdout = stdout;
   bionic_stderr = stderr;
}

#endif
