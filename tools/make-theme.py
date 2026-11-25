#!/usr/bin/env python3
"""Recolour Roblox's app theme in the BloxStation look (PlayStation blue).

Writes the --at-file for rbxm-patch.py. The default theme lives in design-token
modules that come in pairs: a dark one (Surface_0 = 18,18,21) followed by its
light twin (Surface_0 = white), with the same tokens in the same order. Every
Color3.fromRGB(r, g, b) in them is rewritten in place (its three LOADN
instructions):
  - dark greys (backgrounds, surfaces, cards, the side bar) -> navy / blue;
  - mid greys (secondary text, lines) -> slightly blue-tinted greys;
  - Roblox's brand blue -> PlayStation blue;
  - white text and real colours (alerts, green, red ...) stay;
  - the light twin gets the same colours as the dark one, and the explicit
    LightMode tokens get the DarkMode ones, so every page is navy;
  - the raw palette (Extended / Global): dark greys -> navy in the dark set
    (pure black stays, for shadows), flipped in the light set (components
    pick text greys from it for white backgrounds).

usage: make-theme.py BLOB OUT.txt [DARK_OFFSET:LIGHT_OFFSET ...]
  BLOB is rbxm-unpack.py's output of the ORIGINAL UniversalApp.rbxm; without
  offsets the pairs are found by their surface colours.
"""
import colorsys
import importlib.util
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOADN = 4 * 227 % 256  # opcode bytes are stored multiplied by 227
HUE = 216 / 360  # PlayStation blue


def _tool(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def surface0(cols):
    return next((c['rgb'] for c in cols if c['path'] == 'Color.Surface.Surface_0.Color3'), None)


def theme_pairs(d):
    """(dark, light) module offsets of the default theme"""
    finder, reader = _tool('luau-find-module'), _tool('theme-colors')
    mods = []
    for m in re.finditer(rb'Surface_300', d):
        if mods and mods[-1] < m.start() < mods[-1] + 200000 and finder.find(d, m.start(), 200000) == mods[-1]:
            continue
        mod = finder.find(d, m.start())
        if mod is not None and mod not in mods:
            mods.append(mod)
    surf = {mod: surface0(reader.colours(d, mod)) for mod in mods}
    return [(a, b) for a, b in zip(mods, mods[1:])
            if surf[a] == [18, 18, 21] and surf[b] == [255, 255, 255]]


def recolour(rgb):
    r, g, b = rgb
    hi, lo = max(rgb), min(rgb)
    if rgb == [51, 95, 255]:  # Roblox brand blue
        return [0, 112, 240]
    if hi - lo > 24:  # a real colour
        return rgb
    light = (r + g + b) / 3 / 255
    if hi < 100:  # dark grey -> navy / blue
        return [round(v * 255) for v in colorsys.hls_to_rgb(HUE, min(0.10 + light, 0.45), 1.0)]
    if hi < 200:  # mid grey -> blue-tinted grey
        return [round(v * 255) for v in colorsys.hls_to_rgb(HUE, light, 0.30)]
    return rgb  # light grey / white


def flip(rgb):
    """a light-theme palette grey: text greys become light, light greys navy"""
    if max(rgb) - min(rgb) > 24:
        return rgb
    inv = [255 - v for v in rgb]
    if max(inv) < 100:
        return recolour(inv)
    if max(inv) < 200:
        return [round(v * 255) for v in colorsys.hls_to_rgb(HUE, sum(inv) / 3 / 255, 0.30)]
    return inv


def patch(d, lines, colour, new):
    """append the LOADN changes turning colour into new; 1 if anything changed"""
    changed = 0
    for at, old_v, new_v in zip(colour['at'], colour['rgb'], new):
        if old_v != new_v:
            ins = d[at:at + 4]
            assert ins[0] == LOADN and int.from_bytes(ins[2:4], 'little') == old_v, (at, ins.hex())
            lines.append('%d %s %s' % (at, ins.hex(), (ins[:2] + new_v.to_bytes(2, 'little')).hex()))
            changed = 1
    return changed


def theme_lines(d, pairs):
    reader = _tool('theme-colors')
    lines, n = [], 0
    for dark, light in pairs:
        dc, lc = reader.colours(d, dark), reader.colours(d, light)
        assert [c['path'] for c in dc] == [c['path'] for c in lc], 'the two themes differ in shape'
        darkmode = {c['path'][len('DarkMode.'):]: c['rgb'] for c in dc if c['path'].startswith('DarkMode.')}
        for a, b in zip(dc, lc):
            if a['path'].startswith('LightMode.'):
                twin = darkmode.get(a['path'][len('LightMode.'):])
                if twin:
                    new = recolour(twin)
                    n += patch(d, lines, a, new) + patch(d, lines, b, new)
            elif a['path'].startswith(('Color.Extended.', 'Global.')):
                if a['rgb'] != [0, 0, 0]:
                    n += patch(d, lines, a, recolour(a['rgb']))
                n += patch(d, lines, b, flip(b['rgb']))
            else:
                new = recolour(a['rgb'])
                n += patch(d, lines, a, new) + patch(d, lines, b, new)
    return lines, n


def main():
    blob, out = sys.argv[1], sys.argv[2]
    d = open(blob, 'rb').read()
    pairs = [tuple(int(x) for x in a.split(':')) for a in sys.argv[3:]] or theme_pairs(d)
    lines, n = theme_lines(d, pairs)
    Path(out).write_text('\n'.join(lines) + '\n')
    print('%d theme pairs %s: %d colours, %d instructions -> %s' % (len(pairs), pairs, n, len(lines), out))


if __name__ == '__main__':
    main()
