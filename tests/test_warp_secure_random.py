"""Key-generation randomness must fail closed and never use timer jitter."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SecureRandomTest(unittest.TestCase):
    def test_random_device_success_and_failure_clear_output(self):
        source = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "radv/android_probe/warp/secure_random.h"
int main(int argc, char **argv) {
    unsigned char buf[32];
    assert(argc == 3);
    memset(buf, 0x5a, sizeof(buf));
    assert(warp_secure_random_from_path(argv[1], buf, sizeof(buf)) == 0);
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0x2c);
    memset(buf, 0x5a, sizeof(buf));
    assert(warp_secure_random_from_path(argv[2], buf, sizeof(buf)) == -1);
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0);
    memset(buf, 0x5a, sizeof(buf));
    assert(warp_secure_random_from_path("/not-a-real-entropy-device", buf, sizeof(buf)) == -1);
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'test.c').write_text(source)
            (directory / 'entropy').write_bytes(bytes([0x2c]) * 32)
            (directory / 'short').write_bytes(bytes([0x2c]) * 7)
            built = subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                                    str(directory / 'test.c'), '-o', str(directory / 'test')],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(directory / 'test'), str(directory / 'entropy'), str(directory / 'short')],
                           check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
