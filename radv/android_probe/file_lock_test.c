#define _GNU_SOURCE
#include "bionic_file_lock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
   char path[] = "/tmp/bionic-lock-XXXXXX";
   int fd = mkstemp(path);
   assert(fd >= 0);
   unlink(path);
   struct bionic_file_lock lock = {.type = 1, .whence = SEEK_SET,
                                  .start = 1073741824, .len = 1};
   assert(bionic_file_lock(fd, 6, &lock) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0) {
      assert(bionic_file_lock(fd, 5, &lock) == 0);
      assert(lock.type == 1 && lock.pid == getppid());
      assert(lock.start == 1073741824 && lock.len == 1);
      assert(bionic_file_lock(fd, 6, &lock) == -1);
      assert(errno == EACCES || errno == EAGAIN);
      lock.start++;
      assert(bionic_file_lock(fd, 6, &lock) == 0);
      _exit(0);
   }
   int status;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   lock.type = 2;
   assert(bionic_file_lock(fd, 7, &lock) == 0);
   lock.type = 1;
   assert(bionic_file_lock(fd, 5, &lock) == 0 && lock.type == 2);
   assert(bionic_file_lock(fd, 6, NULL) == -1 && errno == EFAULT);
   lock.type = 99;
   assert(bionic_file_lock(fd, 6, &lock) == -1 && errno == EINVAL);
   close(fd);
   puts("PASS: Android locks, contention, range, owner, unlock and invalid inputs");
   return 0;
}
