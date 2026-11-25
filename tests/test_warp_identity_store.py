"""The future first-run identity store must never replace an existing key."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class IdentityStoreTest(unittest.TestCase):
    def test_failed_write_does_not_publish_partial_identity(self):
        source = r'''
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "radv/android_probe/warp/identity_store.h"
ssize_t __wrap_write(int fd, const void *p, size_t n) {
    (void)fd; (void)p; (void)n; errno = EIO; return -1;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(warp_identity_create(argv[1], "secret", 6) == -1);
    assert(access(argv[1], F_OK) == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.c').write_text(source)
            built = subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                str(p / 'test.c'), '-Wl,--wrap=write', '-o', str(p / 'test')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(p / 'test'), str(p / 'identity')], check=True, timeout=5)
            self.assertEqual(sorted(x.name for x in p.iterdir()), ['test', 'test.c'])

    def test_only_one_concurrent_creator_wins(self):
        source = r'''
#include "radv/android_probe/warp/identity_store.h"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    return warp_identity_create(argv[1], argv[2], 1) == 0 ? 0 : 1;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.c').write_text(source)
            built = subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                str(p / 'test.c'), '-o', str(p / 'test')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            jobs = [subprocess.Popen([str(p/'test'), str(p/'identity'), str(i)]) for i in range(8)]
            results = [job.wait(timeout=5) for job in jobs]
            self.assertEqual(results.count(0), 1)
            self.assertEqual(results.count(1), 7)
            self.assertIn((p/'identity').read_text(), [str(i) for i in range(8)])
            self.assertEqual((p/'identity').stat().st_mode & 0o777, 0o600)
            self.assertEqual(sorted(x.name for x in p.iterdir()), ['identity', 'test', 'test.c'])

    def test_create_is_private_and_never_overwrites(self):
        source = r'''
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include "radv/android_probe/warp/identity_store.h"
int main(int argc, char **argv) {
    struct stat st;
    char buf[6] = {0};
    if (argc == 3) {
        assert(warp_identity_create(argv[1], "other", 5) == -1);
        return 0;
    }
    assert(argc == 2);
    umask(0);
    assert(warp_identity_create(argv[1], "first", 5) == 0);
    assert(stat(argv[1], &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(warp_identity_create(argv[1], "other", 5) == -1);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(buf, 1, 5, f) == 5 && fclose(f) == 0);
    assert(memcmp(buf, "first", 5) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.c').write_text('#include <stdio.h>\n' + source)
            exe = p / 'test'
            built = subprocess.run(['cc', '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                                    str(p / 'test.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(exe), str(p / 'identity')], check=True, timeout=5)
            # O_EXCL also refuses a symlink, including a dangling one.
            (p / 'link').symlink_to(p / 'victim')
            subprocess.run([str(exe), str(p / 'link'), 'expect-refusal'], check=True, timeout=5)
            self.assertFalse((p / 'victim').exists())


if __name__ == '__main__':
    unittest.main()
