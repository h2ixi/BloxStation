#!/usr/bin/env python3
"""Draw the on-screen guide's pages (radv/android_probe/roblox_guide.h) and
write them as radv/android_probe/guide_pages.h: each page is a 256-colour image
(run-length coded) that the title copies over Roblox's frames.

usage: make-guide-pages.py [--png DIR]   (--png also saves previews)
Needs Pillow and the Inter font (fonts-inter)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'radv/android_probe/guide_pages.h'
W, H = 1600, 920
FONT_DIR = Path('/usr/share/fonts/opentype/inter')
# the BloxStation look (tools/make-art.py, style psblue): PlayStation blue,
# white blocks
BG_TOP, BG_BOT = (0, 70, 190), (0, 28, 95)
CARD, LINE = (0, 40, 125), (40, 95, 190)
WHITE, GREY, DIM = (255, 255, 255), (205, 220, 255), (150, 178, 230)
BLUE, RED, PINK, GREEN, KEY = (150, 190, 255), (255, 120, 120), (240, 160, 255), (90, 230, 185), (20, 62, 155)


def font(weight, size):
    return ImageFont.truetype(str(FONT_DIR / f'Inter-{weight}.otf'), size)


F_TITLE, F_HEAD, F_KEY, F_TEXT, F_SMALL = font('Bold', 54), font('Bold', 32), font('SemiBold', 26), font('Regular', 26), font('Regular', 24)


def button(d, x, y, name):
    """a controller button at (x, y) (left, centre line); returns its width"""
    r = 21
    shapes = {'X': BLUE, 'O': RED, 'SQ': PINK, 'TRI': GREEN}
    if name in shapes:
        d.ellipse((x, y - r, x + 2 * r, y + r), fill=KEY)
        cx, c = x + r, shapes[name]
        if name == 'X':
            d.line((cx - 9, y - 9, cx + 9, y + 9), fill=c, width=4)
            d.line((cx - 9, y + 9, cx + 9, y - 9), fill=c, width=4)
        elif name == 'O':
            d.ellipse((cx - 10, y - 10, cx + 10, y + 10), outline=c, width=4)
        elif name == 'SQ':
            d.rectangle((cx - 9, y - 9, cx + 9, y + 9), outline=c, width=4)
        else:
            d.polygon([(cx, y - 11), (cx + 11, y + 8), (cx - 11, y + 8)], outline=c, width=4)
        return 2 * r
    w = int(d.textlength(name, font=F_KEY)) + 28
    d.rounded_rectangle((x, y - r, x + w, y + r), radius=r, fill=KEY)
    d.text((x + 14, y), name, font=F_KEY, fill=WHITE, anchor='lm')
    return w


def keys(d, x, y, spec):
    """'X O' -> buttons; '+' between them is drawn as text"""
    for part in spec.split():
        if part in ('+', '/'):
            d.text((x + 4, y), part, font=F_KEY, fill=GREY, anchor='lm')
            x += 26
        else:
            x += button(d, x, y, part) + 8
    return x


def wrap(d, text, width, f):
    lines, cur = [], ''
    for word in text.split():
        t = (cur + ' ' + word).strip()
        if d.textlength(t, font=f) <= width:
            cur = t
        else:
            lines.append(cur)
            cur = word
    return lines + [cur] if cur else lines


def card(d, box, title, rows, key_w=250):
    x0, y0, x1, y1 = box
    d.rounded_rectangle(box, radius=20, fill=CARD)
    d.text((x0 + 28, y0 + 22), title, font=F_HEAD, fill=WHITE)
    y = y0 + 80
    for i, (k, v) in enumerate(rows):
        lines = wrap(d, v, x1 - x0 - key_w - 56, F_TEXT)
        h = max(50, 34 * len(lines) + 14)
        if i:
            d.line((x0 + 28, y - 6, x1 - 28, y - 6), fill=LINE, width=1)
        if k.startswith('='):  # plain words instead of buttons
            d.text((x0 + 28, y + 20), k[1:], font=F_KEY, fill=WHITE, anchor='lm')
        else:
            keys(d, x0 + 28, y + 20, k)
        for j, line in enumerate(lines):
            d.text((x0 + 28 + key_w, y + 20 + 34 * j), line, font=F_TEXT, fill=GREY, anchor='lm')
        y += h + 8


PAGES = [
    ('Welcome to BloxStation', [
        ('Start playing', [
            ('L', 'Move to a game on Home (the D-pad works too)'),
            ('X', 'Open it, then press X on Play'),
            ('=Hold X', 'On a game tile: play it straight away'),
            ('TRI', 'Search for any game'),
            ('Options', 'In a game: the Roblox menu - Leave, Reset, Settings'),
        ]),
        ('This guide', [
            ('=Open it', 'Any time: Guide, in the menu on the left under More'),
            ('L1 / R1', 'Previous / next page (the D-pad left / right too)'),
            ('O', 'Close the guide'),
        ]),
    ]),
    ('Menus', [
        ('Moving around', [
            ('L', 'Move around the page (or the D-pad)'),
            ('X', 'Select'),
            ('O', 'Back'),
            ('TRI', 'Search'),
            ('L1 / R1', 'Switch tabs on the left: Home, Avatar, Chat, More'),
            ('Touchpad', 'A mouse: slide a finger to move, press to click'),
        ]),
        ('The tabs on the left', [
            ('=Home', 'Games for you, friends, continue playing'),
            ('=Avatar', 'Change your character, clothes and emotes'),
            ('=Chat', 'Talk to your friends'),
            ('=More', 'Everything else: see the next page'),
            ('=Guide', 'This guide'),
        ], 160),
    ]),
    ('Under More', [
        ('The boxes', [
            ('=Marketplace', 'Buy clothes, accessories and emotes'),
            ('=Roblox Plus', 'Roblox membership'),
            ('=Profile', 'Your profile page'),
            ('=Friends', 'Your friends and friend requests'),
            ('=Communities', 'Groups you are in'),
            ('=Inventory', 'Everything you own'),
            ('=Messages', 'Your inbox (opens as a web page)'),
            ('=Create', "Roblox Studio's website (making games needs a PC)"),
        ], 230),
        ('', [
            ('=Newsroom', 'Roblox news'),
            ('=Learn', 'Roblox learning pages'),
            ('=Gift Cards', 'Buy Roblox gift cards'),
            ('=Settings', 'Account, privacy, sound and app settings'),
            ('=About', 'Version and terms'),
            ('=Help & Safety', 'Roblox support and safety'),
            ('=Quick Sign-in', 'Sign in on another device with a code'),
            ('=Switch / Log Out', 'Change account, or sign out'),
        ], 250),
    ]),
    ('In a game', [
        ('Controls', [
            ('L', 'Move'),
            ('R', 'Turn the camera'),
            ('X', 'Jump; confirm in game menus'),
            ('SQ TRI O', 'Game actions (each game shows what they do)'),
            ('L2 R2', 'Aim / use a tool, as the game sets them'),
            ('Options', 'Roblox menu: Leave, Reset character, Settings'),
            ('=Tap Touchpad', "The game's menu button, as on a real PS5"),
        ]),
        ('Mouse and extras', [
            ('=Hold Touchpad', 'Hold 1 s: mouse on / off in this game (for PC and phone games). It is remembered'),
            ('=Mouse on', 'Slide a finger to move the pointer, tap to click'),
            ('L3 + R3', 'Hold 1 s: Anti-AFK on / off - the camera turns slowly so you are not kicked'),
            ('=Typing', 'Select a text box and the PS5 keyboard opens'),
        ]),
    ]),
    ('Web pages and connection', [
        ('Web pages', [
            ('=Which', 'Messages, Servers and some Settings pages open as web pages'),
            ('L', 'Move the pointer'),
            ('X', 'Click'),
            ('O', 'Close the page'),
            ('=Servers', "On a game's page: Servers, then Join"),
        ], 180),
        ('Connection and problems', [
            ('=No VPN', 'Roblox uses its own secure tunnel, which gets around most network blocks'),
            ('=Error', 'Press Retry: the PS5 tries another server by itself'),
            ('=Kicked', 'Rejoin from Home; games save your progress'),
            ('=Frozen', 'PS button, close Roblox, start it again'),
        ], 180),
    ]),
]


def mark(d, x, y, s):
    """the BloxStation mark: three white blocks (two below, one on top)"""
    for dx, dy in ((-0.866, 0.5), (0.866, 0.5), (0, -0.5)):
        cx, cy, h = x + dx * s, y + dy * s, s * 0.5
        d.polygon([(cx, cy - s), (cx + s * 0.866, cy - h), (cx, cy), (cx - s * 0.866, cy - h)], fill=(255, 255, 255))
        d.polygon([(cx - s * 0.866, cy - h), (cx, cy), (cx, cy + s), (cx - s * 0.866, cy + h)], fill=(190, 215, 255))
        d.polygon([(cx, cy), (cx + s * 0.866, cy - h), (cx + s * 0.866, cy + h), (cx, cy + s)], fill=(118, 133, 158))


def draw(n, title, cards):
    img = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):  # top to bottom: one colour per row keeps the runs long
        t = y / (H - 1)
        d.line((0, y, W, y), fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOT)))
    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=28, outline=(90, 140, 230), width=3)
    mark(d, 82, 72, 19)
    d.text((132, 36), title, font=F_TITLE, fill=WHITE)
    d.text((W - 48, 66), f'BloxStation   {n + 1} / {len(PAGES)}', font=F_HEAD, fill=DIM, anchor='rm')
    cols = len(cards)
    gap, top, bottom = 28, 130, H - 100
    cw = (W - 96 - gap * (cols - 1)) // cols
    for i, c in enumerate(cards):
        x = 48 + i * (cw + gap)
        card(d, (x, top, x + cw, bottom), c[0], c[1], *(c[2:] or (250,)))
    y = H - 50
    x = keys(d, 48, y, 'L1')
    d.text((x + 6, y), 'previous', font=F_SMALL, fill=DIM, anchor='lm')
    x = keys(d, x + 140, y, 'R1')
    d.text((x + 6, y), 'next page', font=F_SMALL, fill=DIM, anchor='lm')
    x = keys(d, W - 210, y, 'O')
    d.text((x + 6, y), 'close', font=F_SMALL, fill=DIM, anchor='lm')
    return img


def encode(img):
    """256 colours; runs of up to 256 pixels as (length - 1, colour) byte pairs"""
    q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = (q.getpalette() + [0] * 768)[:768]
    px = q.tobytes()
    runs = bytearray()
    i = 0
    while i < len(px):
        j = i
        while j < len(px) and px[j] == px[i] and j - i < 256:
            j += 1
        runs += bytes((j - i - 1, px[i]))
        i = j
    return pal, bytes(runs)


def main():
    png = sys.argv[sys.argv.index('--png') + 1] if '--png' in sys.argv else None
    out = ['/* generated by tools/make-guide-pages.py - do not edit */',
           '#ifndef GUIDE_PAGES_H', '#define GUIDE_PAGES_H',
           f'#define GUIDE_W {W}', f'#define GUIDE_H {H}', f'#define GUIDE_PAGES {len(PAGES)}']
    total = 0
    for n, (title, cards) in enumerate(PAGES):
        img = draw(n, title, cards)
        if png:
            Path(png).mkdir(parents=True, exist_ok=True)
            img.save(f'{png}/guide-{n + 1}.png')
        pal, runs = encode(img)
        total += len(runs)
        out.append(f'static const uint8_t guide_pal{n}[768] = {{{",".join(map(str, pal))}}};')
        body = ','.join(map(str, runs))
        out.append(f'static const uint8_t guide_rle{n}[{len(runs)}] = {{{body}}};')
    out.append('static const uint8_t *const guide_pal[GUIDE_PAGES] = {%s};' %
               ','.join(f'guide_pal{n}' for n in range(len(PAGES))))
    out.append('static const uint8_t *const guide_rle[GUIDE_PAGES] = {%s};' %
               ','.join(f'guide_rle{n}' for n in range(len(PAGES))))
    out.append('static const uint32_t guide_rle_len[GUIDE_PAGES] = {%s};' %
               ','.join(f'sizeof(guide_rle{n})' for n in range(len(PAGES))))
    out.append('#endif')
    OUT.write_text('\n'.join(out) + '\n')
    print(f'{OUT.relative_to(ROOT)}: {len(PAGES)} pages, {total} bytes')


main()
