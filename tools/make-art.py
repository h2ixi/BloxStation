#!/usr/bin/env python3
"""BloxStation art for the PS5 title: icon0.png (512x512) and pic0/pic1.dds
(3840x2160 BC7 with a DX10 header), drawn from scratch (no Roblox or other
third-party artwork). Several styles; sce_sys/bloxstation/ holds the chosen one.

usage: make-art.py STYLE OUT_DIR        one style, ready for the title (+ banner.png)
       make-art.py --preview OUT_DIR    a small preview of every style
Needs numpy, Pillow and the Inter font (fonts-inter)."""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = Path('/usr/share/fonts/opentype/inter')
W, H = 3840, 2160


def font(weight, size):
    return ImageFont.truetype(str(FONT / f'Inter-{weight}.otf'), size)


def gradient(w, h, top, bot, glow_at=(0.5, 0.45), glow=(25, 60, 150), spread=(0.38, 0.32)):
    y = np.linspace(0, 1, h)[:, None]
    x = np.linspace(0, 1, w)[None, :]
    g = np.exp(-(((x - glow_at[0]) / spread[0]) ** 2 + ((y - glow_at[1]) / spread[1]) ** 2))[..., None]
    img = np.array(top, float) * (1 - y[..., None]) + np.array(bot, float) * y[..., None] + g * np.array(glow, float)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB').convert('RGBA')


def cube(d, cx, cy, s, top, side, k=1.0, outline=None):
    """isometric cube centred at (cx, cy), edge s; faces top / left / right"""
    h = s * 0.5
    sh = lambda c, m: tuple(int(min(255, v * m * k)) for v in c)
    faces = ([(cx, cy - s), (cx + s * 0.866, cy - h), (cx, cy), (cx - s * 0.866, cy - h)], sh(top, 1)), \
            ([(cx - s * 0.866, cy - h), (cx, cy), (cx, cy + s), (cx - s * 0.866, cy + h)], sh(side, 1)), \
            ([(cx, cy), (cx + s * 0.866, cy - h), (cx + s * 0.866, cy + h), (cx, cy + s)], sh(side, 0.62))
    for poly, col in faces:
        d.polygon(poly, fill=col, outline=outline, width=max(1, int(s / 30)) if outline else 1)


def stack(size, top, side, outline=None):
    """the mark: three blocks, two below and one on top"""
    im = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s, c = size * 0.2, size / 2
    for dx, dy in ((-0.866, 0.5), (0.866, 0.5), (0, -0.5)):
        cube(d, c + dx * s, c + dy * s + s * 0.15, s, top, side, outline=outline)
    return im


def shadowed(base, im, x, y, blur, alpha=0.55, off=(0.02, 0.03)):
    a = np.array(im)
    a[..., :3] = 0
    a[..., 3] = (a[..., 3] * alpha).astype(np.uint8)
    sh = Image.new('RGBA', base.size, (0, 0, 0, 0))
    sh.alpha_composite(Image.fromarray(a, 'RGBA'), (int(x + im.width * off[0]), int(y + im.height * off[1])))
    base.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))
    base.alpha_composite(im, (int(x), int(y)))


def centered(d, y, text, f, fill, w):
    d.text(((w - d.textlength(text, font=f)) / 2, y), text, font=f, fill=fill)


def pattern(w, h, s, top, side, alpha):
    p = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(p)
    for row in range(-1, int(h / (s * 1.5)) + 2):
        for col in range(-1, int(w / (s * 1.732)) + 2):
            cube(d, col * s * 1.732 + (row % 2) * s * 0.866, row * s * 1.5, s * 0.9, top, side)
    a = np.array(p)
    a[..., 3] = (a[..., 3] * alpha).astype(np.uint8)
    return Image.fromarray(a, 'RGBA')


def standard(top_bg, bot_bg, glow, block_top, block_side, text, sub, pat_alpha=0.05, outline=None, upper=False):
    """layout shared by the styles: mark above the name"""
    name = 'BLOXSTATION' if upper else 'BloxStation'
    icon = gradient(512, 512, top_bg, bot_bg, (0.5, 0.42), glow, (0.55, 0.5))
    icon.alpha_composite(stack(360, block_top, block_side, outline), (76, 36))
    d = ImageDraw.Draw(icon)
    centered(d, 400, name, font('ExtraBold', 56 if upper else 64), text, 512)
    pic = gradient(W, H, top_bg, bot_bg, (0.5, 0.42), glow)
    if pat_alpha:
        pic.alpha_composite(pattern(W, H, 90, block_top, block_side, pat_alpha))
    mark = stack(900, block_top, block_side, outline)
    shadowed(pic, mark, (W - 900) / 2, H * 0.16, 30)
    d = ImageDraw.Draw(pic)
    centered(d, H * 0.16 + 900, name, font('Black', 270 if upper else 300), text, W)
    centered(d, H * 0.16 + 1280, 'Roblox on PlayStation 5', font('Regular', 110), sub, W)
    return icon, pic


STYLES = {
    # navy night, blue and cyan blocks
    'navy': lambda: standard((14, 22, 52), (3, 6, 18), (25, 60, 150), (60, 210, 255), (40, 110, 255),
                             (245, 248, 255), (170, 195, 235)),
    # PlayStation blue, white blocks
    'psblue': lambda: standard((0, 70, 190), (0, 28, 95), (40, 90, 200), (255, 255, 255), (190, 215, 255),
                               (255, 255, 255), (210, 225, 255), pat_alpha=0.07),
    # black with neon cyan and magenta
    'neon': lambda: standard((10, 8, 20), (2, 2, 6), (90, 20, 120), (0, 255, 230), (230, 40, 255),
                             (240, 240, 255), (190, 160, 230), pat_alpha=0.04),
    # red and black, bold capitals
    'red': lambda: standard((40, 6, 10), (8, 2, 3), (150, 20, 25), (255, 80, 80), (220, 20, 35),
                            (255, 245, 245), (240, 180, 180), upper=True),
    # light: white background, dark blocks, no pattern
    'light': lambda: standard((245, 247, 252), (215, 222, 235), (0, 0, 0), (40, 50, 70), (20, 25, 40),
                              (20, 25, 40), (90, 100, 120), pat_alpha=0),
    # sunset orange and purple
    'sunset': lambda: standard((70, 20, 90), (20, 5, 35), (200, 70, 40), (255, 190, 60), (255, 110, 60),
                               (255, 245, 230), (240, 200, 210), pat_alpha=0.05),
}

if sys.argv[1] == '--preview':
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    tiles = []
    for name, make in STYLES.items():
        icon, pic = make()
        icon.convert('RGB').save(out / f'{name}-icon.png')
        small = pic.convert('RGB').resize((1920, 1080), Image.LANCZOS)
        small.save(out / f'{name}-background.png')
        t = Image.new('RGB', (1920 + 40 + 512, 1080), (30, 30, 30))
        t.paste(small, (0, 0))
        t.paste(icon.convert('RGB').resize((512, 512)), (1960, 284))
        tiles.append((name, t))
    sheet = Image.new('RGB', (2472, 1180 * len(tiles)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for i, (name, t) in enumerate(tiles):
        d.text((20, i * 1180 + 20), f'{i + 1}. {name}', font=font('Bold', 60), fill=(255, 255, 255))
        sheet.paste(t, (0, i * 1180 + 100))
    sheet.resize((sheet.width // 2, sheet.height // 2), Image.LANCZOS).save(out / 'all-styles.png')
    print('previews:', ', '.join(STYLES))
    sys.exit(0)

style, out = sys.argv[1], Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
icon, pic = STYLES[style]()
icon.convert('RGB').save(out / 'icon0.png')
pic.convert('RGB').save(out / 'pic-preview.png')
# the GitHub page banner (1280x640, the social preview size)
banner = pic.convert('RGB').resize((1280, 720), Image.LANCZOS).crop((0, 40, 1280, 680))
banner.save(out / 'banner.png', optimize=True)

# ---- BC7 mode 6 encoder (one subset, RGBA 7-bit endpoints + p-bit, 4-bit
# indices) ------------------------------------------------------------------
px = np.array(pic.convert('RGBA'), dtype=np.int32)
bh, bw = H // 4, W // 4
blk = px.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
lo, hi = blk.min(axis=1), blk.max(axis=1)
q0 = lo >> 1
q1 = np.maximum(hi - 1, 0) >> 1
e0 = (q0 << 1).astype(np.float64)
e1 = ((q1 << 1) | 1).astype(np.float64)
dv = e1 - e0
den = (dv * dv).sum(axis=1)
den[den == 0] = 1
t = ((blk - e0[:, None, :]) * dv[:, None, :]).sum(axis=2) / den[:, None]
weights = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]) / 64.0
idx = np.abs(np.clip(t, 0, 1)[..., None] - weights).argmin(axis=2)
p0 = np.zeros(len(blk), np.int64)
p1 = np.ones(len(blk), np.int64)
swap = idx[:, 0] >= 8
q0s, q1s = q0.copy(), q1.copy()
q0[swap], q1[swap] = q1s[swap], q0s[swap]
p0[swap], p1[swap] = 1, 0
idx[swap] = 15 - idx[swap]
lo64 = np.zeros(len(blk), np.uint64)
hi64 = np.zeros(len(blk), np.uint64)
pos = 0


def put(v, n):
    global pos, lo64, hi64
    v = v.astype(np.uint64) & np.uint64((1 << n) - 1)
    for b in range(n):
        bit = (v >> np.uint64(b)) & np.uint64(1)
        if pos < 64:
            lo64 |= bit << np.uint64(pos)
        else:
            hi64 |= bit << np.uint64(pos - 64)
        pos += 1


put(np.full(len(blk), 1 << 6), 7)
for ch in range(4):
    put(q0[:, ch], 7)
    put(q1[:, ch], 7)
put(p0, 1)
put(p1, 1)
put(idx[:, 0], 3)
for i in range(1, 16):
    put(idx[:, i], 4)
assert pos == 128
data = np.stack([lo64, hi64], axis=1).astype('<u8').tobytes()
assert len(data) == W * H

# DDS header (124 bytes after the magic) + DX10 header: BC7_UNORM (98), 2D
flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps height width pixelformat mipmapcount linearsize
head = b'DDS ' + struct.pack('<7I', 124, flags, H, W, len(data), 1, 1) + b'\0' * 44
head += struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)  # pixel format: fourcc
head += struct.pack('<5I', 0x1000, 0, 0, 0, 0)  # caps: texture
head += struct.pack('<5I', 98, 3, 0, 1, 1)  # DXGI BC7_UNORM, texture2D, 1 array, alpha straight
assert len(head) == 148
for n in ('pic0.dds', 'pic1.dds'):
    (out / n).write_bytes(head + data)

w = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64])
E0 = (q0 << 1) | p0[:, None]
E1 = (q1 << 1) | p1[:, None]
ww = w[idx][..., None]
rec = ((64 - ww) * E0[:, None, :] + ww * E1[:, None, :] + 32) >> 6
rec = rec.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4)
print('BC7 mean abs error per channel:', np.abs(rec - px).mean(axis=(0, 1)).round(2))
Image.fromarray(rec[..., :3].astype(np.uint8)).resize((1920, 1080)).save(out / 'pic-decoded.png')
print('wrote', sorted(p.name for p in out.iterdir()))
