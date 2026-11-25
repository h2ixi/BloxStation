/* The engine's spill files, kept in memory.
 *
 * While a game loads or streams, the engine (WriteOnlyBuffer) writes each
 * downloaded asset to a file under cache/wob or cache/http-wob, reads it back
 * and deletes it, or renames it into its HTTP cache. On the PS5's storage
 * those calls took 3-10 s of every 10 s while a game streamed, single ones up
 * to 0.7 s, on threads the frame waits for: the remaining freezes, in every
 * game.
 *
 * Here those files live in memory. The engine gets a real FILE opened on
 * VF_DUMMY (so a stdio call this layer does not take over does nothing
 * harmful), and every stdio shim asks vf_find() first. File data is a
 * reference-counted blob per name: unlink drops the name, open handles keep
 * reading. Past VF_CAP bytes of live files new ones go to storage as before;
 * a rename to a name outside the spill folders writes the file out there.
 *
 * Plain C and libc only: radv/android_probe/vfile_test.c tests it on the PC. */
#ifndef BIONIC_VFILE_H
#define BIONIC_VFILE_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VF_DUMMY
#define VF_DUMMY "/dev/null"
#endif
#define VF_NAMES 4096
#define VF_HANDLES 1024

struct vf_blob {
   char *data;
   size_t len, cap;
   unsigned refs; /* its name (if any) + open handles */
};
static struct {
   char path[192];
   struct vf_blob *blob; /* NULL: free */
} vf_names[VF_NAMES];
static struct {
   FILE *f; /* the dummy FILE the engine holds; NULL: free */
   int fd;  /* fileno(f) */
   struct vf_blob *blob;
   size_t pos;
   int writable, eof, err;
} vf_h[VF_HANDLES];
static pthread_mutex_t vf_lock = PTHREAD_MUTEX_INITIALIZER;
static int vf_live; /* open handles: 0 makes vf_find free */
static size_t vf_bytes, vf_cap = 128u << 20;
static uint64_t vf_kept, vf_spilled, vf_written_out;

static int
vf_is_spill(const char *path)
{
   return path && strlen(path) < sizeof(vf_names[0].path) &&
          (strstr(path, "/roblox/cache/wob/") || strstr(path, "/roblox/cache/http-wob/"));
}
static void
vf_unref(struct vf_blob *b) /* under vf_lock */
{
   if (b && --b->refs == 0) {
      free(b->data);
      free(b);
   }
}
static int
vf_name(const char *path) /* under vf_lock; -1 none */
{
   for (int i = 0; i < VF_NAMES; i++)
      if (vf_names[i].blob && strcmp(vf_names[i].path, path) == 0)
         return i;
   return -1;
}
static void
vf_drop_name(int i) /* under vf_lock */
{
   vf_bytes -= vf_names[i].blob->len;
   vf_unref(vf_names[i].blob);
   vf_names[i].blob = NULL;
   vf_names[i].path[0] = 0;
}
static int
vf_is_named(const struct vf_blob *b) /* under vf_lock */
{
   for (int i = 0; i < VF_NAMES; i++)
      if (vf_names[i].blob == b)
         return 1;
   return 0;
}

/* the handle of f, or -1 (most FILEs: one load and out) */
static int
vf_find(FILE *f)
{
   if (!__atomic_load_n(&vf_live, __ATOMIC_ACQUIRE) || !f)
      return -1;
   pthread_mutex_lock(&vf_lock);
   int i = VF_HANDLES - 1;
   while (i >= 0 && vf_h[i].f != f)
      i--;
   pthread_mutex_unlock(&vf_lock);
   return i;
}
static int
vf_find_fd(int fd)
{
   if (!__atomic_load_n(&vf_live, __ATOMIC_ACQUIRE) || fd < 0)
      return -1;
   pthread_mutex_lock(&vf_lock);
   int i = VF_HANDLES - 1;
   while (i >= 0 && !(vf_h[i].f && vf_h[i].fd == fd))
      i--;
   pthread_mutex_unlock(&vf_lock);
   return i;
}

/* fopen of a spill file: a memory file, or NULL for storage */
static FILE *
vf_open(const char *path, const char *mode)
{
   if (!vf_cap || !vf_is_spill(path) || !mode)
      return NULL;
   const int plus = strchr(mode, '+') != NULL;
   const char m = mode[0];
   if (m != 'r' && m != 'w' && m != 'a')
      return NULL;
   pthread_mutex_lock(&vf_lock);
   int n = vf_name(path);
   struct vf_blob *b = NULL;
   if (m == 'r') {
      if (n < 0) { /* not one of ours (left on storage by an older run) */
         pthread_mutex_unlock(&vf_lock);
         return NULL;
      }
      b = vf_names[n].blob;
   } else if (m == 'a' && n >= 0) {
      b = vf_names[n].blob;
   } else {
      if (n >= 0)
         vf_drop_name(n); /* "w": a new file under that name */
      n = -1;
      if (vf_bytes < vf_cap)
         for (int i = 0; i < VF_NAMES && n < 0; i++)
            if (!vf_names[i].blob)
               n = i;
      b = n >= 0 ? calloc(1, sizeof(*b)) : NULL;
      if (!b) {
         pthread_mutex_unlock(&vf_lock);
         vf_spilled++;
         return NULL;
      }
      b->refs = 1;
      snprintf(vf_names[n].path, sizeof(vf_names[n].path), "%s", path);
      vf_names[n].blob = b;
      vf_kept++;
   }
   int h = 0;
   while (h < VF_HANDLES && vf_h[h].f)
      h++;
   FILE *f = h < VF_HANDLES ? fopen(VF_DUMMY, "r") : NULL;
   if (!f) {
      if (m == 'w' && b->refs == 1 && b->len == 0)
         vf_drop_name(n);
      pthread_mutex_unlock(&vf_lock);
      return NULL;
   }
   b->refs++;
   vf_h[h].f = f;
   vf_h[h].fd = fileno(f);
   vf_h[h].blob = b;
   vf_h[h].pos = m == 'a' ? b->len : 0;
   vf_h[h].writable = m != 'r' || plus;
   vf_h[h].eof = vf_h[h].err = 0;
   __atomic_add_fetch(&vf_live, 1, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&vf_lock);
   return f;
}

static size_t
vf_write(int h, const void *buf, size_t n)
{
   pthread_mutex_lock(&vf_lock);
   if (!vf_h[h].writable) {
      vf_h[h].err = 1;
      pthread_mutex_unlock(&vf_lock);
      errno = EBADF;
      return 0;
   }
   struct vf_blob *b = vf_h[h].blob;
   const size_t end = vf_h[h].pos + n;
   if (end > b->cap) {
      size_t cap = b->cap ? b->cap : 16384;
      while (cap < end)
         cap *= 2;
      char *d = realloc(b->data, cap);
      if (!d) {
         vf_h[h].err = 1;
         pthread_mutex_unlock(&vf_lock);
         errno = ENOMEM;
         return 0;
      }
      b->data = d;
      b->cap = cap;
   }
   if (vf_h[h].pos > b->len)
      memset(b->data + b->len, 0, vf_h[h].pos - b->len);
   memcpy(b->data + vf_h[h].pos, buf, n);
   if (end > b->len) {
      if (vf_is_named(b))
         vf_bytes += end - b->len;
      b->len = end;
   }
   vf_h[h].pos = end;
   pthread_mutex_unlock(&vf_lock);
   return n;
}
static size_t
vf_read(int h, void *buf, size_t n)
{
   pthread_mutex_lock(&vf_lock);
   const struct vf_blob *b = vf_h[h].blob;
   size_t m = vf_h[h].pos < b->len ? b->len - vf_h[h].pos : 0;
   if (m > n)
      m = n;
   memcpy(buf, b->data + vf_h[h].pos, m);
   vf_h[h].pos += m;
   if (m < n)
      vf_h[h].eof = 1;
   pthread_mutex_unlock(&vf_lock);
   return m;
}
static int
vf_getc(int h)
{
   unsigned char c;
   return vf_read(h, &c, 1) == 1 ? c : EOF;
}
static char *
vf_gets(int h, char *s, int size)
{
   if (size <= 0)
      return NULL;
   int k = 0;
   while (k < size - 1) {
      const int c = vf_getc(h);
      if (c == EOF)
         break;
      s[k++] = (char)c;
      if (c == '\n')
         break;
   }
   if (!k)
      return NULL;
   s[k] = 0;
   return s;
}
static int
vf_seek(int h, int64_t off, int whence)
{
   pthread_mutex_lock(&vf_lock);
   const int64_t base = whence == SEEK_SET ? 0
                        : whence == SEEK_CUR ? (int64_t)vf_h[h].pos
                                             : (int64_t)vf_h[h].blob->len;
   const int64_t at = base + off;
   if (at < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
      pthread_mutex_unlock(&vf_lock);
      errno = EINVAL;
      return -1;
   }
   vf_h[h].pos = (size_t)at;
   vf_h[h].eof = 0;
   pthread_mutex_unlock(&vf_lock);
   return 0;
}
static int64_t
vf_tell(int h)
{
   pthread_mutex_lock(&vf_lock);
   const int64_t p = (int64_t)vf_h[h].pos;
   pthread_mutex_unlock(&vf_lock);
   return p;
}
static int vf_eof(int h) { return vf_h[h].eof; }
static int vf_error(int h) { return vf_h[h].err; }
static void vf_clearerr(int h) { vf_h[h].eof = vf_h[h].err = 0; }
/* bytes [off, off+n) of handle h's file into buf (zeros past its end), for
 * mmap of its descriptor: the engine maps spill files to read them */
static void
vf_pread(int h, int64_t off, void *buf, size_t n)
{
   pthread_mutex_lock(&vf_lock);
   const struct vf_blob *b = vf_h[h].blob;
   size_t m = off >= 0 && (size_t)off < b->len ? b->len - (size_t)off : 0;
   if (m > n)
      m = n;
   if (m)
      memcpy(buf, b->data + off, m);
   memset((char *)buf + m, 0, n - m);
   pthread_mutex_unlock(&vf_lock);
}
static int64_t
vf_size_fd(int h)
{
   pthread_mutex_lock(&vf_lock);
   const int64_t n = (int64_t)vf_h[h].blob->len;
   pthread_mutex_unlock(&vf_lock);
   return n;
}
static int
vf_truncate_fd(int h, int64_t len)
{
   if (len < 0) {
      errno = EINVAL;
      return -1;
   }
   pthread_mutex_lock(&vf_lock);
   struct vf_blob *b = vf_h[h].blob;
   if ((size_t)len > b->cap) {
      char *d = realloc(b->data, (size_t)len);
      if (!d) {
         pthread_mutex_unlock(&vf_lock);
         errno = ENOMEM;
         return -1;
      }
      b->data = d;
      b->cap = (size_t)len;
   }
   if ((size_t)len > b->len)
      memset(b->data + b->len, 0, (size_t)len - b->len);
   if (vf_is_named(b))
      vf_bytes = vf_bytes - b->len + (size_t)len;
   b->len = (size_t)len;
   pthread_mutex_unlock(&vf_lock);
   return 0;
}
static int
vf_close(int h)
{
   pthread_mutex_lock(&vf_lock);
   FILE *f = vf_h[h].f;
   vf_unref(vf_h[h].blob);
   vf_h[h].f = NULL;
   vf_h[h].blob = NULL;
   __atomic_sub_fetch(&vf_live, 1, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&vf_lock);
   fclose(f);
   return 0;
}

/* unlink / stat / rename of a spill file held here: 1 handled */
static int
vf_unlink(const char *path)
{
   if (!vf_is_spill(path))
      return 0;
   pthread_mutex_lock(&vf_lock);
   const int n = vf_name(path);
   if (n >= 0)
      vf_drop_name(n);
   pthread_mutex_unlock(&vf_lock);
   return n >= 0;
}
static int
vf_stat(const char *path, int64_t *size)
{
   if (!vf_is_spill(path))
      return 0;
   pthread_mutex_lock(&vf_lock);
   const int n = vf_name(path);
   if (n >= 0)
      *size = (int64_t)vf_names[n].blob->len;
   pthread_mutex_unlock(&vf_lock);
   return n >= 0;
}
/* rc: 0 not ours; 1 done (*result is rename's return value) */
static int
vf_rename(const char *from, const char *to, int *result)
{
   if (!vf_is_spill(from))
      return 0;
   pthread_mutex_lock(&vf_lock);
   const int n = vf_name(from);
   if (n < 0) {
      pthread_mutex_unlock(&vf_lock);
      return 0;
   }
   struct vf_blob *b = vf_names[n].blob;
   if (vf_is_spill(to)) {
      const int old = vf_name(to);
      if (old >= 0 && old != n)
         vf_drop_name(old);
      snprintf(vf_names[n].path, sizeof(vf_names[n].path), "%s", to);
      pthread_mutex_unlock(&vf_lock);
      *result = 0;
      return 1;
   }
   /* out of the spill folders (the HTTP cache): write it there */
   b->refs++;
   pthread_mutex_unlock(&vf_lock);
   FILE *out = fopen(to, "wb");
   int ok = out && (b->len == 0 || fwrite(b->data, 1, b->len, out) == b->len);
   if (out && fclose(out) != 0)
      ok = 0;
   pthread_mutex_lock(&vf_lock);
   vf_unref(b);
   if (ok) {
      const int again = vf_name(from);
      if (again >= 0)
         vf_drop_name(again);
      vf_written_out++;
   }
   pthread_mutex_unlock(&vf_lock);
   *result = ok ? 0 : -1;
   return 1;
}
static void
vf_stats(uint64_t *kept, uint64_t *spilled, uint64_t *written_out, size_t *live)
{
   pthread_mutex_lock(&vf_lock);
   *kept = vf_kept;
   *spilled = vf_spilled;
   *written_out = vf_written_out;
   *live = vf_bytes;
   pthread_mutex_unlock(&vf_lock);
}

#endif
