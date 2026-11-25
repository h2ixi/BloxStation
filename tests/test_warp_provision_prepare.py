"""Private per-console key preparation must be atomic and non-replacing."""
import base64
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WG = ROOT / 'third_party/wireguard-lwip/src'

class WarpPrepareTest(unittest.TestCase):
    def test_keypair_is_private_and_never_replaced(self):
        code = r'''
#include <assert.h>
#include "radv/android_probe/warp/provision_prepare.h"
int main(int argc, char **argv) {
    assert(argc == 3);
    assert(warp_provision_prepare(argv[1], argv[2]) == 0);
    assert(warp_provision_prepare(argv[1], argv[2]) == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'test.c').write_text(code)
            raw = bytes.fromhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
            (p/'entropy').write_bytes(raw)
            cmd = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-stringop-overread', '-I', str(ROOT), '-I', str(WG),
                   str(p/'test.c'), str(WG/'crypto/refc/x25519.c'), str(WG/'crypto.c'), '-o', str(p/'test')]
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(p/'test'), str(p/'entropy'), str(p/'pending')], check=True, timeout=5)
            d = json.loads((p/'pending').read_text())
            self.assertEqual(base64.b64decode(d['private_key']), raw)
            self.assertEqual(base64.b64decode(d['public_key']).hex(),
                '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
            self.assertEqual((p/'pending').stat().st_mode & 0o777, 0o600)

if __name__ == '__main__':
    unittest.main()
