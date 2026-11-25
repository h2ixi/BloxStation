"""Test the public gateway-address parser without a PS5 or network."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class GatewayConfigTest(unittest.TestCase):
    def test_addresses_must_be_explicit_and_well_formed(self):
        src = r'''
#include <assert.h>
#include <netinet/in.h>
#include "radv/android_probe/gateway_config.h"
int main(void) {
    struct in_addr ip;
    unsigned short port;
    assert(gateway_config_parse(NULL, &ip, &port) == -1);
    assert(gateway_config_parse("", &ip, &port) == -1);
    assert(gateway_config_parse("127.0.0.1:1080", &ip, &port) == 0 && port == 1080);
    assert(gateway_config_parse("192.0.2.1", &ip, &port) == 0 && port == 1080);
    assert(gateway_config_parse("192.0.2.1:65536", &ip, &port) == -1);
    assert(gateway_config_parse("192.0.2.1:0", &ip, &port) == -1);
    assert(gateway_config_parse("192.0.2.1:80junk", &ip, &port) == -1);
    assert(gateway_config_parse("hostname:1080", &ip, &port) == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            cfile.write_text(src)
            result = subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                                     str(cfile), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
