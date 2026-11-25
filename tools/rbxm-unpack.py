#!/usr/bin/env python3
"""Decompress every chunk of a binary Roblox model (.rbxm) into one blob, so
its strings and Luau bytecode can be searched (read only: the engine checks
the models' signatures, so they cannot be changed).

usage: rbxm-unpack.py IN.rbxm OUT.bin
"""
import struct
import sys
from compression import zstd


def lz4_block(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        n = tok >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255:
                    break
        out += src[i:i + n]; i += n
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8; i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255:
                    break
        m += 4
        start = len(out) - off
        for k in range(m):
            out.append(out[start + k])
    return bytes(out[:size])


d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'<roblox!', 'not a binary model'
p = 32
out = bytearray()
while p + 16 <= len(d):
    name = d[p:p + 4]
    clen, ulen = struct.unpack_from('<II', d, p + 4)
    p += 16
    if clen == 0:
        body = d[p:p + ulen]; p += ulen
    else:
        raw = d[p:p + clen]; p += clen
        body = zstd.decompress(raw) if raw[:4] == b'\x28\xb5\x2f\xfd' else lz4_block(raw, ulen)
    out += name + b'\0' + body
    if name == b'END\0':
        break
open(sys.argv[2], 'wb').write(out)
print(len(out), 'bytes')
