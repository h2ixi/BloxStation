/* First-run WARP identity storage. Never replace an existing identity.
 * Caller must provide a trusted, private directory. */
#ifndef ROBLOX_WARP_IDENTITY_STORE_H
#define ROBLOX_WARP_IDENTITY_STORE_H
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Create path exclusively (O_EXCL: never replaces, never follows a symlink),
 * mode 0600, write all data, fsync file and directory. On a write or sync
 * failure the file this call created is removed, so no partial identity is
 * left behind. A crash mid-write can still leave a short file: readers must
 * validate the content and treat an invalid file as needing recovery.
 * Only open/write/fsync/close/unlink are used: the PS5 title has no
 * mkstemp and no link() import (a bare mkstemp call crashed the title). */
static int
warp_identity_create(const char *path, const void *data, size_t size)
{
   if (!path || !*path || !data || !size)
      return -1;
   const size_t n = strlen(path);
   if (n > 4096)
      return -1;
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (fd < 0)
      return -1;
   const unsigned char *p = data;
   size_t done = 0;
   int ok = 1;
   while (done < size) {
      const ssize_t wrote = write(fd, p + done, size - done);
      if (wrote < 0 && errno == EINTR)
         continue;
      if (wrote <= 0) {
         ok = 0;
         break;
      }
      done += (size_t)wrote;
   }
   if (ok && fsync(fd) != 0)
      ok = 0;
   if (close(fd) != 0)
      ok = 0;
   if (!ok) {
      unlink(path); /* created exclusively by this call */
      return -1;
   }
   char *dir = malloc(n + 2);
   if (dir) {
      memcpy(dir, path, n + 1);
      char *slash = strrchr(dir, '/');
      if (!slash)
         memcpy(dir, ".", 2);
      else if (slash == dir)
         slash[1] = 0;
      else
         *slash = 0;
      const int dfd = open(dir, O_RDONLY);
      if (dfd >= 0) {
         (void)fsync(dfd); /* best effort: some filesystems refuse it */
         close(dfd);
      }
      free(dir);
   }
   return 0;
}
#endif
