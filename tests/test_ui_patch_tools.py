"""BloxStation UI patch tools: theme colour reader, recolour rule, rbxm patcher."""
import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), TOOLS / (name + '.py'))
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def varint(n):
    out = bytearray()
    while True:
        b, n = n & 0x7f, n >> 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def ins(op, a=0, b=0, c=0, d=None):
    lo = (op * 227) & 0xff
    if d is not None:
        return struct.pack('<I', lo | a << 8 | (d & 0xffff) << 16)
    return struct.pack('<I', lo | a << 8 | b << 16 | c << 24)


def token_module(surface0):
    """a Luau module building {Surface = {Color3 = Color3.fromRGB(*surface0)}}
    and {Text = {Color3 = Color3.fromRGB(200, 40, 40)}} (a real colour)"""
    strings = ['Color3', 'fromRGB', 'Surface', 'Text']
    k = {s: i for i, s in enumerate(strings)}  # constant i = string i
    code = b''
    code += ins(53, a=0) + struct.pack('<I', 0)             # NEWTABLE R0
    for key, rgb in (('Surface', surface0), ('Text', (200, 40, 40))):
        code += ins(53, a=1) + struct.pack('<I', 0)         # NEWTABLE R1
        code += ins(12, a=2) + struct.pack('<I', 0)         # GETIMPORT R2
        for i, v in enumerate(rgb):
            code += ins(4, a=3 + i, d=v)                    # LOADN R3..R5
        code += ins(21, a=2, b=4, c=2)                      # CALL R2
        code += ins(16, a=2, b=1) + struct.pack('<I', k['Color3'])
        code += ins(16, a=1, b=0) + struct.pack('<I', k[key])
    code += ins(22, a=0, b=1)                               # RETURN
    ncode = len(code) // 4
    m = bytes([6, 3]) + varint(len(strings))
    for s in strings:
        m += varint(len(s)) + s.encode()
    m += b'\0'                                              # no userdata types
    m += varint(1)                                          # one function
    m += bytes([8, 0, 0, 0]) + b'\0' + varint(0)            # header, flags, no types
    m += varint(ncode) + code
    m += varint(len(strings)) + b''.join(b'\x03' + varint(i + 1) for i in range(len(strings)))
    m += varint(0) + varint(0) + varint(0) + b'\0' + b'\0'  # children, line, name, no debug
    m += varint(0)                                          # main function
    return m


class ThemeColoursTest(unittest.TestCase):
    def test_reads_paths_values_and_offsets(self):
        tc = load('theme-colors')
        blob = b'pad' + token_module((18, 18, 21))
        cols = tc.colours(blob, 3)
        self.assertEqual([c['path'] for c in cols], ['Surface.Color3', 'Text.Color3'])
        self.assertEqual(cols[0]['rgb'], [18, 18, 21])
        for c in cols:  # each offset is a LOADN holding that value
            for at, v in zip(c['at'], c['rgb']):
                self.assertEqual(blob[at], (4 * 227) & 0xff)
                self.assertEqual(int.from_bytes(blob[at + 2:at + 4], 'little'), v)


class RecolourTest(unittest.TestCase):
    def setUp(self):
        self.mt = load('make-theme')

    def test_rule(self):
        r = self.mt.recolour
        navy = r([18, 18, 21])
        self.assertTrue(navy[2] > navy[1] > navy[0] and max(navy) < 120)   # dark grey -> navy
        self.assertEqual(r([255, 255, 255]), [255, 255, 255])               # white text stays
        self.assertEqual(r([200, 40, 40]), [200, 40, 40])                   # real colours stay
        self.assertEqual(r([51, 95, 255]), [0, 112, 240])                   # brand blue
        self.assertGreater(sum(r([39, 41, 48])), sum(navy))                 # brighter stays brighter
        self.assertEqual(self.mt.flip([32, 34, 39]), [223, 221, 216])       # dark text -> light
        self.assertLess(max(self.mt.flip([242, 244, 245])), 120)            # light surface -> navy

    def test_pair_gets_the_same_navy(self):
        dark, light = token_module((18, 18, 21)), token_module((255, 255, 255))
        blob = b'p' + dark + light
        lines, n = self.mt.theme_lines(blob, [(1, 1 + len(dark))])
        self.assertEqual(n, 2)  # Surface in both, Text untouched
        out = bytearray(blob)
        for line in lines:
            at, old, new = line.split()
            at, old, new = int(at), bytes.fromhex(old), bytes.fromhex(new)
            self.assertEqual(out[at:at + 4], old)
            out[at:at + 4] = new
        tc = load('theme-colors')
        want = self.mt.recolour([18, 18, 21])
        for off in (1, 1 + len(dark)):
            cols = {c['path']: c['rgb'] for c in tc.colours(bytes(out), off)}
            self.assertEqual(cols, {'Surface.Color3': want, 'Text.Color3': [200, 40, 40]})


class RbxmPatchTest(unittest.TestCase):
    def test_at_file_and_checksum(self):
        body = b'\x00' * 8 + b'hello world, this is chunk data'
        rbxm = b'<roblox!' + b'\0' * 24
        rbxm += b'PROP' + struct.pack('<III', 0, len(body), 0) + body
        rbxm += b'END\0' + struct.pack('<III', 0, 0, 0) + b'</roblox>'
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / 'in.rbxm').write_bytes(rbxm)
            subprocess.run([sys.executable, TOOLS / 'rbxm-unpack.py', t / 'in.rbxm', t / 'blob'],
                           check=True, capture_output=True)
            at = (t / 'blob').read_bytes().index(b'world')
            (t / 'at.txt').write_text('%d %s %s\n' % (at, b'world'.hex(), b'WORLD'.hex()))
            (t / 'out').mkdir()
            subprocess.run([sys.executable, TOOLS / 'rbxm-patch.py', t / 'in.rbxm', t / 'out/UniversalApp.rbxm',
                            '--at-file=%s' % (t / 'at.txt')], check=True, capture_output=True)
            out = (t / 'out/UniversalApp.rbxm').read_bytes()
            self.assertEqual((t / 'out/UniversalApp_checksum').read_text(), hashlib.blake2b(out).hexdigest())
            subprocess.run([sys.executable, TOOLS / 'rbxm-unpack.py', t / 'out/UniversalApp.rbxm', t / 'blob2'],
                           check=True, capture_output=True)
            self.assertIn(b'hello WORLD, this', (t / 'blob2').read_bytes())
            # a wrong expected value is refused
            (t / 'at.txt').write_text('%d %s %s\n' % (at, b'xxxxx'.hex(), b'WORLD'.hex()))
            r = subprocess.run([sys.executable, TOOLS / 'rbxm-patch.py', t / 'in.rbxm', t / 'out/x.rbxm',
                                '--at-file=%s' % (t / 'at.txt')], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('expected', r.stderr)


if __name__ == '__main__':
    unittest.main()
