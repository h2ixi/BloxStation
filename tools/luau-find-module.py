#!/usr/bin/env python3
"""Find the start of the Luau bytecode module whose string table holds a given
string, in a blob from rbxm-unpack.py. Prints the offset of the version byte.
usage: luau-find-module.py BLOB STRING_OFFSET
Also importable: find(data, offset) -> offset or None."""
import sys


def _varint(d, p):
    r = s = 0
    while True:
        b = d[p]
        p += 1
        r |= (b & 0x7f) << s
        s += 7
        if not b & 0x80:
            return r, p


def _holds(d, start, target):
    """does a string table starting at start (version byte) contain target?"""
    n, p = _varint(d, start + 2)
    if not 1 <= n <= 100000:
        return False
    for _ in range(n):
        ln, p = _varint(d, p)
        if ln == 0 or ln > 100000:
            return False
        if any(c < 9 or (13 < c < 32) for c in d[p:p + ln]):
            return False
        if p <= target < p + ln:
            return True
        p += ln
        if p > target + 200000:
            return False
    return False


def find(d, target, window=4000000):
    for start in range(target - 1, max(0, target - window), -1):
        if d[start] in (3, 4, 5, 6, 7, 8, 9) and d[start + 1] in (1, 2, 3):
            try:
                if _holds(d, start, target):
                    return start
            except IndexError:
                pass
    return None


if __name__ == '__main__':
    r = find(open(sys.argv[1], 'rb').read(), int(sys.argv[2]))
    print('not found' if r is None else r)
