/* Host test of bionic_vfile.h: cc -std=gnu11 -Wall -Wextra -Werror
 * -fsanitize=address,undefined -pthread vfile_test.c && ./a.out */
#include "bionic_vfile.h"

#include <assert.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                            \
   do {                                                                                                    \
      if (!(c)) {                                                                                          \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                      \
         failures++;                                                                                       \
      }                                                                                                    \
   } while (0)

static const char *W = "/x/roblox/cache/wob/wob-1";
static const char *H = "/x/roblox/cache/http-wob/wob-2";

static void *
worker(void *arg)
{
   char path[96], buf[64];
   snprintf(path, sizeof(path), "/x/roblox/cache/wob/thread-%ld", (long)(intptr_t)arg);
   for (int k = 0; k < 300; k++) {
      FILE *f = vf_open(path, "wb");
      if (!f)
         continue;
      const int h = vf_find(f);
      snprintf(buf, sizeof(buf), "%ld:%d", (long)(intptr_t)arg, k);
      vf_write(h, buf, strlen(buf));
      vf_close(h);
      f = vf_open(path, "rb");
      if (f) {
         const int r = vf_find(f);
         char got[64] = {0};
         vf_read(r, got, sizeof(got) - 1);
         if (strcmp(got, buf) != 0)
            __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
         vf_close(r);
      }
      vf_unlink(path);
   }
   return NULL;
}

int
main(void)
{
   /* not spill paths: left to storage */
   CHECK(vf_open("/x/roblox/cache/http/abc", "wb") == NULL);
   CHECK(vf_open("/x/roblox/files/a.txt", "wb") == NULL);
   CHECK(vf_open(W, "rb") == NULL); /* not written yet */
   CHECK(vf_find(stdout) == -1);

   /* write, read back */
   FILE *f = vf_open(W, "wb");
   CHECK(f != NULL);
   int h = vf_find(f);
   CHECK(h >= 0);
   CHECK(vf_write(h, "hello ", 6) == 6);
   CHECK(vf_write(h, "world\nline2\n", 12) == 12);
   CHECK(vf_tell(h) == 18);
   int64_t size = -1;
   CHECK(vf_stat(W, &size) && size == 18);
   CHECK(vf_size_fd(vf_find_fd(fileno(f))) == 18);
   {
      char mapped[24];
      vf_pread(h, 6, mapped, sizeof(mapped)); /* "world\nline2\n" then zeros */
      CHECK(memcmp(mapped, "world\nline2\n", 12) == 0 && mapped[12] == 0 && mapped[23] == 0);
   }
   CHECK(vf_close(h) == 0);
   CHECK(vf_find(f) == -1);

   FILE *r = vf_open(W, "rb");
   CHECK(r != NULL);
   int rh = vf_find(r);
   char buf[64] = {0};
   CHECK(vf_gets(rh, buf, sizeof(buf)) && strcmp(buf, "hello world\n") == 0);
   CHECK(vf_getc(rh) == 'l');
   CHECK(vf_seek(rh, -2, SEEK_END) == 0 && vf_tell(rh) == 16);
   memset(buf, 0, sizeof(buf));
   CHECK(vf_read(rh, buf, 10) == 2 && strcmp(buf, "2\n") == 0);
   CHECK(vf_eof(rh));
   vf_clearerr(rh);
   CHECK(!vf_eof(rh));
   CHECK(vf_write(rh, "x", 1) == 0 && vf_error(rh)); /* read-only */
   /* unlink while open: the name goes, the open handle keeps reading */
   CHECK(vf_unlink(W) == 1);
   CHECK(!vf_stat(W, &size));
   CHECK(vf_open(W, "rb") == NULL);
   CHECK(vf_seek(rh, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(vf_read(rh, buf, 5) == 5 && strcmp(buf, "hello") == 0);
   CHECK(vf_close(rh) == 0);
   CHECK(vf_unlink(W) == 0); /* already gone */

   /* "w" truncates an existing name; seek past the end fills zeros */
   f = vf_open(H, "wb");
   h = vf_find(f);
   vf_write(h, "abcdef", 6);
   vf_close(h);
   f = vf_open(H, "wb");
   h = vf_find(f);
   CHECK(vf_seek(h, 4, SEEK_SET) == 0);
   vf_write(h, "Z", 1);
   CHECK(vf_truncate_fd(h, 8) == 0);
   vf_close(h);
   CHECK(vf_stat(H, &size) && size == 8);
   r = vf_open(H, "rb");
   rh = vf_find(r);
   unsigned char raw[8];
   CHECK(vf_read(rh, raw, 8) == 8 && raw[0] == 0 && raw[4] == 'Z' && raw[7] == 0);
   vf_close(rh);

   /* rename inside the spill folders */
   int res = -1;
   CHECK(vf_rename(H, W, &res) == 1 && res == 0);
   CHECK(!vf_stat(H, &size) && vf_stat(W, &size) && size == 8);
   /* rename out of them: written to that file */
   char tmpl[] = "/tmp/vfile-test-XXXXXX";
   const int fd = mkstemp(tmpl);
   CHECK(fd >= 0);
   close(fd);
   CHECK(vf_rename(W, tmpl, &res) == 1 && res == 0);
   CHECK(!vf_stat(W, &size));
   FILE *disk = fopen(tmpl, "rb");
   CHECK(disk && fread(raw, 1, 8, disk) == 8 && raw[4] == 'Z');
   if (disk)
      fclose(disk);
   unlink(tmpl);
   CHECK(vf_rename("/x/other/file", W, &res) == 0);

   /* append */
   f = vf_open(W, "wb");
   h = vf_find(f);
   vf_write(h, "ab", 2);
   vf_close(h);
   f = vf_open(W, "ab");
   h = vf_find(f);
   vf_write(h, "cd", 2);
   vf_close(h);
   CHECK(vf_stat(W, &size) && size == 4);
   vf_unlink(W);

   /* the cap: past it, new files go to storage */
   vf_cap = 1024;
   f = vf_open(W, "wb");
   h = vf_find(f);
   char big[2048] = {0};
   vf_write(h, big, sizeof(big));
   vf_close(h);
   CHECK(vf_open(H, "wb") == NULL);
   vf_unlink(W);
   CHECK(vf_bytes == 0);
   vf_cap = 128u << 20;

   /* threads */
   pthread_t t[8];
   for (long i = 0; i < 8; i++)
      pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);
   for (int i = 0; i < 8; i++)
      pthread_join(t[i], NULL);
   CHECK(vf_bytes == 0);
   CHECK(__atomic_load_n(&vf_live, __ATOMIC_RELAXED) == 0);

   uint64_t kept, spilled, out;
   size_t live;
   vf_stats(&kept, &spilled, &out, &live);
   printf("vfile: kept %llu, spilled %llu, written out %llu, live %zu; %d failures\n", (unsigned long long)kept,
          (unsigned long long)spilled, (unsigned long long)out, live, failures);
   return failures != 0;
}
