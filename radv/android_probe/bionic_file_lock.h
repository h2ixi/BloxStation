/* Android x86_64 flock uses Linux field order and lock constants.
 * Keep this conversion independent of the console runtime for host testing. */
#ifndef BIONIC_FILE_LOCK_H
#define BIONIC_FILE_LOCK_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>

struct bionic_file_lock {
   int16_t type, whence;
   int32_t padding;
   int64_t start, len;
   int32_t pid, padding2;
};
_Static_assert(sizeof(struct bionic_file_lock) == 32, "Android flock size");
_Static_assert(offsetof(struct bionic_file_lock, start) == 8, "Android flock offset");

static int
bionic_file_lock(int fd, int cmd, struct bionic_file_lock *in)
{
   if (!in) {
      errno = EFAULT;
      return -1;
   }
   struct flock lock = {0};
   switch (in->type) {
   case 0: lock.l_type = F_RDLCK; break;
   case 1: lock.l_type = F_WRLCK; break;
   case 2: lock.l_type = F_UNLCK; break;
   default: errno = EINVAL; return -1;
   }
   lock.l_whence = in->whence;
   lock.l_start = in->start;
   lock.l_len = in->len;
   lock.l_pid = in->pid;
   int native_cmd;
   switch (cmd) {
   case 5: native_cmd = F_GETLK; break;
   case 6: native_cmd = F_SETLK; break;
   case 7: native_cmd = F_SETLKW; break;
   default: errno = EINVAL; return -1;
   }
   const int rc = fcntl(fd, native_cmd, &lock);
   if (rc == 0 && cmd == 5) {
      in->type = lock.l_type == F_RDLCK ? 0 : lock.l_type == F_WRLCK ? 1 : 2;
      in->whence = lock.l_whence;
      in->start = lock.l_start;
      in->len = lock.l_len;
      in->pid = lock.l_pid;
   }
   return rc;
}
#endif
