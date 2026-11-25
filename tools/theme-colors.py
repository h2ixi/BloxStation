#!/usr/bin/env python3
"""List the Color3.fromRGB(r, g, b) colours of a Luau module (in a blob from
rbxm-unpack.py) with their token paths and the blob offsets of their three
LOADN instructions, one JSON object per line.

usage: theme-colors.py BLOB MODULE_OFFSET
Every function of the module is scanned; a colour's path is the chain of
table keys it is stored under (e.g. Color.Surface.Surface_0.Color3).
Also importable: colours(data, offset) -> [{'path', 'rgb', 'at'}].
"""
import json
import struct
import sys

INV = pow(227, -1, 256)  # opcode bytes are stored multiplied by 227
LOADN, MOVE, GETIMPORT, SETTABLEKS, CALL, NEWTABLE, DUPTABLE = 4, 6, 12, 16, 21, 53, 54
AUX = {7, 8, 12, 15, 16, 20, 27, 28, 29, 30, 31, 32, 53, 55, 58, 59, 66, 71, 72, 77, 78, 79, 80}


def _protos(d, p):
    """[(code offset, code words, constants)] of every function of the module"""
    def varint():
        nonlocal p
        r = s = 0
        while True:
            b = d[p]
            p += 1
            r |= (b & 0x7f) << s
            s += 7
            if not b & 0x80:
                return r

    ver, tver = d[p], d[p + 1]
    p += 2
    strings = []
    for _ in range(varint()):
        ln = varint()
        strings.append(d[p:p + ln].decode('utf-8', 'replace'))
        p += ln
    if tver == 3:  # userdata type remapping: (index, name) pairs until 0
        while d[p]:
            p += 1
            varint()
        p += 1
    protos = []
    for _ in range(varint()):
        p += 4  # maxstack, numparams, nups, is_vararg
        if ver >= 4:
            p += 1  # flags
            size = varint()
            p += size  # type info
        ncode = varint()
        code_at = p
        code = [int.from_bytes(d[p + 4 * i:p + 4 * i + 4], 'little') for i in range(ncode)]
        p += 4 * ncode
        consts = []
        for _ in range(varint()):
            t = d[p]
            p += 1
            v = None
            if t == 1:
                v = bool(d[p])
                p += 1
            elif t == 2:
                v = struct.unpack_from('<d', d, p)[0]
                p += 8
            elif t == 3:
                v = strings[varint() - 1]
            elif t == 4:
                p += 4
            elif t == 5:
                for _ in range(varint()):
                    varint()
            elif t == 6:
                varint()
            elif t == 7:
                p += 16
            elif t == 8:
                for _ in range(varint()):
                    varint()
                    p += 4
            elif t != 0:
                sys.exit('unknown constant type %d' % t)
            consts.append(v)
        for _ in range(varint()):  # child protos
            varint()
        varint()  # line defined
        varint()  # debug name
        if d[p]:  # line info
            gap = d[p + 1]
            p += 2 + ncode + 4 * (((ncode - 1) >> gap) + 1)
        else:
            p += 1
        if d[p]:  # debug info
            p += 1
            for _ in range(varint()):
                varint(); varint(); varint()
                p += 1
            for _ in range(varint()):
                varint()
        else:
            p += 1
        protos.append((code_at, code, consts))
    return protos


def _scan(code_at, code, consts, out):
    # registers hold node ids; a node is a table or a colour; SETTABLEKS links
    parent, reg, colours = {}, {}, {}
    nodes = 0
    pending = None  # (register of the GETIMPORT, [(value, offset) of its LOADNs])
    i = 0
    while i < len(code):
        ins = code[i]
        op = ((ins & 0xff) * INV) & 0xff
        a, b = (ins >> 8) & 0xff, (ins >> 16) & 0xff
        dv = ins >> 16
        if dv >= 0x8000:
            dv -= 0x10000
        aux = code[i + 1] if op in AUX and i + 1 < len(code) else None
        if op in (DUPTABLE, NEWTABLE):
            nodes += 1
            reg[a] = nodes
        elif op == GETIMPORT:
            pending = (a, [])
        elif op == LOADN and pending and a == pending[0] + 1 + len(pending[1]):
            pending[1].append((dv, code_at + 4 * i))
        elif op == CALL and pending and a == pending[0] and len(pending[1]) == 3:
            nodes += 1
            colours[nodes] = pending[1]
            reg[a] = nodes
            pending = None
        elif op == SETTABLEKS and a in reg and b in reg and aux is not None:
            parent[reg[a]] = (reg[b], consts[aux & 0xffffff])
        elif op == MOVE and b in reg:
            reg[a] = reg[b]
        i += 2 if aux is not None else 1
    for n, vals in colours.items():
        keys = []
        while n in parent:
            n, k = parent[n]
            keys.append(str(k))
        out.append({'path': '.'.join(reversed(keys)), 'rgb': [v for v, _ in vals], 'at': [o for _, o in vals]})


def colours(data, offset):
    out = []
    for code_at, code, consts in _protos(data, offset):
        _scan(code_at, code, consts, out)
    return out


if __name__ == '__main__':
    for c in colours(open(sys.argv[1], 'rb').read(), int(sys.argv[2])):
        print(json.dumps(c))
