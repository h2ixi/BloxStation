"""Key generation uses fail-closed entropy and interoperable X25519."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
WG = ROOT / 'third_party/wireguard-lwip/src'


class WarpKeypairTest(unittest.TestCase):
    def test_known_vector_and_failure_zeroes_both_keys(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "radv/android_probe/warp/keypair.h"
int main(int argc, char **argv) {
    unsigned char priv[32], pub[32];
    const unsigned char expected[32] = {
      0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
      0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a};
    assert(argc == 2);
    assert(warp_keypair_from_entropy(argv[1], priv, pub) == 0);
    assert(memcmp(pub, expected, 32) == 0);
    memset(priv, 0xff, 32); memset(pub, 0xff, 32);
    assert(warp_keypair_from_entropy("/not-an-entropy-device", priv, pub) == -1);
    for (int i = 0; i < 32; i++) assert(priv[i] == 0 && pub[i] == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.c').write_text(source)
            # RFC 7748 example scalar, not a real key.
            (p / 'entropy').write_bytes(bytes.fromhex(
                '77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a'))
            command = ['cc', '-std=c99', '-D_DEFAULT_SOURCE', '-I', str(ROOT), '-I', str(WG), str(p / 'test.c'),
                       str(WG / 'crypto/refc/x25519.c'), str(WG / 'crypto.c'), '-o', str(p / 'test')]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(p / 'test'), str(p / 'entropy')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
