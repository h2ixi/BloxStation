#!/usr/bin/env python3
"""Change strings inside a binary Roblox model (.rbxm), e.g. a constant in a
ModuleScript's Luau bytecode, and write it back with its checksum file.

The app checks UniversalApp.rbxm only against UniversalApp_checksum (BLAKE2b-512
of the file, hex), which is rewritten here. Untouched chunks are
copied byte for byte; changed ones are stored zstd-compressed.

A replacement is OLD=NEW (Python bytes escapes allowed). It applies to a Luau
string-table entry: OLD must appear as one whole string (its varint length
right before it), and every such entry in a PROP string value is replaced; the
value's u32 length is fixed up, so NEW may be longer or shorter.

A replacement written --bytes=OLDHEX=NEWHEX changes raw bytes instead (same
length; e.g. one bytecode instruction). Each must match exactly once.

--at-file=F changes bytes at offsets of the rbxm-unpack.py blob: one
"OFFSET OLDHEX NEWHEX" per line (same length; OLDHEX must be there), e.g. from
make-theme.py.

usage: rbxm-patch.py IN.rbxm OUT.rbxm 'OLD=NEW' [...] [--bytes=OLDHEX=NEWHEX ...] [--at-file=F]
   --only-with=S   only change bytecode values that also contain S (bytes)
"""
import ast
import hashlib
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


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def unesc(s):
    return ast.literal_eval("b'" + s.replace("'", "\\'") + "'")


args = [a for a in sys.argv[1:] if not a.startswith('--')]
raw_reps = [tuple(bytes.fromhex(x) for x in a.split('=', 1)[1].split('='))
            for a in sys.argv[1:] if a.startswith('--bytes=')]
for o, n in raw_reps:
    assert len(o) == len(n), 'a --bytes replacement must keep the length'
at = sorted((int(o), bytes.fromhex(x), bytes.fromhex(y))
            for a in sys.argv[1:] if a.startswith('--at-file=')
            for o, x, y in (l.split() for l in open(a.split('=', 1)[1]) if l.strip()))
for _, x, y in at:
    assert len(x) == len(y), 'an --at-file change must keep the length'
at_done = 0
only = [unesc(a.split('=', 1)[1]) for a in sys.argv[1:] if a.startswith('--only-with=')]
src, dst = args[0], args[1]
reps = []
for r in args[2:]:
    o, n = r.split('=', 1)
    reps.append((unesc(o), unesc(n)))

d = open(src, 'rb').read()
assert d[:8] == b'<roblox!'
out = bytearray(d[:32])
p = 32
blob = 0  # offset of this chunk's body in the rbxm-unpack.py blob
done = {o: 0 for o, _ in reps + raw_reps}
while p + 16 <= len(d):
    name = d[p:p + 4]
    clen, ulen, res = struct.unpack_from('<III', d, p + 4)
    orig = d[p:p + 16 + (clen or ulen)]
    p += 16
    if clen == 0:
        body = d[p:p + ulen]; p += ulen
    else:
        raw = d[p:p + clen]; p += clen
        body = zstd.decompress(raw) if raw[:4] == b'\x28\xb5\x2f\xfd' else lz4_block(raw, ulen)
    changed = False
    blob += 5
    here = [(o - blob, x, y) for o, x, y in at if blob <= o < blob + len(body)]
    if here:
        nb = bytearray(body)
        for o, x, y in here:
            if nb[o:o + len(x)] != x:
                sys.exit('--at-file %d: found %s, expected %s' % (o + blob, nb[o:o + len(x)].hex(), x.hex()))
            nb[o:o + len(y)] = y
        at_done += len(here)
        body = bytes(nb)
        changed = True
    blob += len(body)
    if name == b'PROP' and any(o in body for o, _ in reps + raw_reps):
        cls, nl = struct.unpack_from('<II', body, 0)
        pname = body[8:8 + nl]
        q = 8 + nl
        typ = body[q]; q += 1
        if typ in (1, 29):  # string / bytecode values: u32 length + bytes, one per instance
            vals = []
            while q + 4 <= len(body):
                ln = struct.unpack_from('<I', body, q)[0]
                vals.append(body[q + 4:q + 4 + ln]); q += 4 + ln
            assert q == len(body), 'PROP string parse did not end at the chunk end'
            nv = []
            for v in vals:
                if not only or all(s in v for s in only):
                    for o, n in reps:
                        key = varint(len(o)) + o
                        if key in v:
                            c = v.count(key)
                            v = v.replace(key, varint(len(n)) + n)
                            done[o] += c
                            print('%s: %d x %r -> %r in a %d-byte %s value' % (pname.decode(), c, o, n, len(v), pname.decode()))
                    for o, n in raw_reps:
                        if o in v:
                            c = v.count(o)
                            v = v.replace(o, n)
                            done[o] += c
                            print('%s: %d x bytes %s -> %s' % (pname.decode(), c, o.hex(), n.hex()))
                nv.append(v)
            nb = body[:9 + nl] + b''.join(struct.pack('<I', len(v)) + v for v in nv)
            changed = changed or nb != body
            body = nb
    if not changed:
        out += orig  # untouched chunks byte for byte (SIGN must stay as it is)
    else:
        z = zstd.compress(body, 10)
        out += name + struct.pack('<III', len(z), len(body), res) + z
    if name == b'END\0':
        out += d[p:]  # the footer
        break
for o, c in done.items():
    if not c:
        sys.exit('not found: %r' % o)
    if any(o == r for r, _ in raw_reps) and c != 1:
        sys.exit('--bytes %s matched %d times, not once' % (o.hex(), c))
if at_done != len(at):
    sys.exit('--at-file: %d of %d offsets were inside a chunk' % (at_done, len(at)))
if at:
    print('%d --at-file changes' % len(at))
open(dst, 'wb').write(out)
cs = hashlib.blake2b(bytes(out)).hexdigest()
open(dst.rsplit('/', 1)[0] + '/UniversalApp_checksum' if '/' in dst else 'UniversalApp_checksum', 'w').write(cs)
print('wrote', dst, len(out), 'bytes; checksum', cs[:16], '...')
