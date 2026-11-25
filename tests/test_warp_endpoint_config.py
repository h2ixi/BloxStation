"""WARP must use the registration endpoint unless explicitly overridden."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


class WarpEndpointTest(unittest.TestCase):
    def test_registration_and_override_validation(self):
        code = r'''
#include <assert.h>
#include <string.h>
#include "radv/android_probe/warp/endpoint_config.h"
int main(void) {
    char ip[64]; unsigned short port;
    assert(warp_endpoint_port("2408", &port) == 0 && port == 2408);
    assert(warp_endpoint_port("2408junk", &port) == -1);
    assert(warp_endpoint_port("", &port) == -1);
    assert(warp_endpoint_port("0", &port) == -1);
    assert(warp_endpoint_port("65536", &port) == -1);
    assert(warp_endpoint_select("162.159.192.7", 2408, NULL, ip, sizeof(ip), &port) == 0);
    assert(strcmp(ip, "162.159.192.7") == 0 && port == 2408);
    assert(warp_endpoint_select("162.159.192.7", 2408, "188.114.98.1:4500", ip, sizeof(ip), &port) == 0);
    assert(strcmp(ip, "188.114.98.1") == 0 && port == 4500);
    assert(warp_endpoint_select("invalid", 2408, NULL, ip, sizeof(ip), &port) == -1);
    assert(warp_endpoint_select("162.159.192.7", 0, NULL, ip, sizeof(ip), &port) == -1);
    assert(warp_endpoint_select("162.159.192.7", 2408, "188.114.98.1:70000", ip, sizeof(ip), &port) == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.c').write_text(code)
            built = subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                                    str(p / 'test.c'), '-o', str(p / 'test')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(p / 'test')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
