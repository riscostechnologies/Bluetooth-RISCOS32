#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 RISC OS Technologies
"""Generate the Bluetooth desktop sprite files (32bpp, 1bpp mask).

Usage: mksprites.py <output-directory>
Writes host-named files with ,ff9 suffixes:
  !Bluetooth/!Sprites, !Sprites22   (!bluetooth, sm!bluetooth, bt_idle, bt_busy, bt_on)
  !BTSetup/!Sprites, !Sprites22      (!btsetup, sm!btsetup)
  !BTSetup/CoSprite, CoSprite22      (co_bluetooth)
"""
import math
import os
import struct
import sys

BLUE = (0x00, 0x7F, 0xE8)
GREY = (0x8C, 0x8C, 0x8C)
AMBER = (0xE8, 0x96, 0x00)
WHITE = (0xFF, 0xFF, 0xFF)
SAMPLES = 4


def seg_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


RUNE = [
    ((0.50, 0.14), (0.50, 0.86)),
    ((0.50, 0.14), (0.70, 0.32)),
    ((0.70, 0.32), (0.30, 0.68)),
    ((0.50, 0.86), (0.70, 0.68)),
    ((0.70, 0.68), (0.30, 0.32)),
]


def render(size_x, size_y, colour, small=False):
    """Return rows (top first) of (rgb, solid) for the logo."""
    rows = []
    stroke = 0.06 if small else 0.042
    for y in range(size_y):
        row = []
        for x in range(size_x):
            inside = rune = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    u = (x + (sx + 0.5) / SAMPLES) / size_x
                    v = (y + (sy + 0.5) / SAMPLES) / size_y
                    # A pill: ellipse 0.66 wide, 0.96 tall.
                    ex = (u - 0.5) / 0.33
                    ey = (v - 0.5) / 0.48
                    if ex * ex + ey * ey <= 1.0:
                        inside += 1
                        if min(seg_distance(u, v, *a, *b) for a, b in RUNE) <= stroke:
                            rune += 1
            total = SAMPLES * SAMPLES
            solid = inside * 2 >= total
            if inside:
                w = rune / inside
                rgb = tuple(int(round(colour[i] * (1 - w) + WHITE[i] * w)) for i in range(3))
            else:
                rgb = (0, 0, 0)
            row.append((rgb, solid))
        rows.append(row)
    return rows


def squash(rows):
    """Halve the height for 90x45 dpi files."""
    out = []
    for i in range(0, len(rows) - 1, 2):
        a, b = rows[i], rows[i + 1]
        row = []
        for (ca, sa), (cb, sb) in zip(a, b):
            rgb = tuple((ca[k] + cb[k]) // 2 for k in range(3)) if sa and sb else (ca if sa else cb)
            row.append((rgb, sa or sb))
        out.append(row)
    return out


def sprite(name, rows, ydpi):
    height = len(rows)
    width = len(rows[0])
    mask_words = (width + 31) // 32
    image = bytearray()
    mask = bytearray()
    for row in rows:
        for (r, g, b), _ in row:
            image += struct.pack('<I', r | (g << 8) | (b << 16))
        bits = [0] * mask_words
        for x, (_, solid) in enumerate(row):
            if solid:
                bits[x // 32] |= 1 << (x % 32)
        for word in bits:
            mask += struct.pack('<I', word)
    mode = (6 << 27) | (ydpi << 14) | (90 << 1) | 1
    header_size = 44
    image_offset = header_size
    mask_offset = header_size + len(image)
    total = mask_offset + len(mask)
    header = struct.pack('<I12s7i', total, name.encode('latin-1')[:12].ljust(12, b'\0'),
                         width - 1, height - 1, 0, 31, image_offset, mask_offset, mode)
    return header + bytes(image) + bytes(mask)


def sprite_alpha(name, rows, ydpi=90):
    """RISC OS 5 32bpp sprite with an 8bpp alpha mask (mode word bit 31),
    as the Shield theme's co_ icons use.  rows: (r, g, b, a) top first."""
    height = len(rows)
    width = len(rows[0])
    image = bytearray()
    mask = bytearray()
    for row in rows:
        for r, g, b, a in row:
            image += struct.pack('<I', r | (g << 8) | (b << 16))
        line = bytes(a for _, _, _, a in row)
        mask += line + b'\0' * ((-len(line)) % 4)
    mode = (1 << 31) | (6 << 27) | (ydpi << 14) | (90 << 1) | 1
    mask_offset = 44 + len(image)
    total = mask_offset + len(mask)
    header = struct.pack('<I12s7I', total, name.encode('latin-1')[:12].ljust(12, b'\0'),
                         width - 1, height - 1, 0, 31, 44, mask_offset, mode)
    return header + bytes(image) + bytes(mask)


def configure_icon(base_path):
    """The Shield jigsaw piece with its cog replaced by a Bluetooth badge."""
    data = open(base_path, 'rb').read()
    width, height = data[0], data[1]
    px = [[list(data[2 + (y * width + x) * 4:6 + (y * width + x) * 4])
           for x in range(width)] for y in range(height)]

    def is_emblem(p):
        r, g, b, a = p
        return a > 40 and b > r + 40
    # The cog plus a one pixel border (its anti-aliased and shadow edge).
    emblem = {(x, y) for y in range(height) for x in range(width) if is_emblem(px[y][x])}
    grown = set(emblem)
    for x, y in emblem:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                q = (x + dx, y + dy)
                if 0 <= q[0] < width and 0 <= q[1] < height and px[q[1]][q[0]][3] > 200:
                    grown.add(q)
    # Fill it by diffusing the surrounding grey inwards.
    for x, y in grown:
        px[y][x][:3] = [0, 0, 0]
    known = {(x, y) for y in range(height) for x in range(width)
             if px[y][x][3] > 200 and (x, y) not in grown}
    todo = set(grown)
    while todo:
        done = {}
        for x, y in todo:
            near = [px[y + dy][x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                                                      (1, 1), (-1, -1), (1, -1), (-1, 1))
                    if (x + dx, y + dy) in known]
            if near:
                done[(x, y)] = [sum(p[i] for p in near) // len(near) for i in range(3)]
        if not done:
            break
        for (x, y), rgb in done.items():
            px[y][x][:3] = rgb
            known.add((x, y))
        todo -= set(done)
    # Badge: a blue pill with a white rune, shaded like the theme's cog.
    cx, cy, rx, ry = 21.0, 21.5, 7.8, 11.6
    top, bottom = (120, 176, 236), (36, 84, 160)
    edge = (28, 52, 96)
    for y in range(height):
        for x in range(width):
            inside = rune = 0
            outline = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    u = x + (sx + 0.5) / SAMPLES
                    v = y + (sy + 0.5) / SAMPLES
                    ex, ey = (u - cx) / rx, (v - cy) / ry
                    d = ex * ex + ey * ey
                    if d <= 1.0:
                        inside += 1
                        if d > 0.80:
                            outline += 1
                        uu = (u - (cx - rx)) / (2 * rx) * 0.66 + 0.17
                        vv = (v - (cy - ry)) / (2 * ry) * 0.96 + 0.02
                        if min(seg_distance(uu, vv, *a, *b) for a, b in RUNE) <= 0.05:
                            rune += 1
            if not inside:
                continue
            total = SAMPLES * SAMPLES
            t = (y - (cy - ry)) / (2 * ry)
            fill = [top[i] * (1 - t) + bottom[i] * t for i in range(3)]
            o = outline / inside
            fill = [fill[i] * (1 - o) + edge[i] * o for i in range(3)]
            w = rune / inside
            fill = [fill[i] * (1 - w) + 255 * w for i in range(3)]
            cover = inside / total
            base = px[y][x]
            px[y][x][:3] = [int(round(fill[i] * cover + base[i] * (1 - cover))) for i in range(3)]
    return [[tuple(p) for p in row] for row in px]


ACTIVE = (0x00, 0x7F, 0xE8)
INACTIVE = (0x9A, 0x9C, 0xA6)


def _rr(u, v, x0, y0, x1, y1, r):
    cx = max(x0 + r, min(u, x1 - r))
    cy = max(y0 + r, min(v, y1 - r))
    return (u - cx) ** 2 + (v - cy) ** 2 <= r * r


def _circle(u, v, cx, cy, r):
    return (u - cx) ** 2 + (v - cy) ** 2 <= r * r


def _line(u, v, ax, ay, bx, by, t):
    return seg_distance(u, v, ax, ay, bx, by) <= t


def glyph_speaker(u, v):
    return (_rr(u, v, .30, .17, .70, .83, .07) and not _circle(u, v, .5, .60, .13)
            and not _circle(u, v, .5, .34, .055))


def glyph_headset(u, v):
    d = math.hypot(u - .5, v - .56)
    band = .22 <= d <= .285 and v <= .56
    return band or _rr(u, v, .20, .52, .33, .76, .045) or _rr(u, v, .67, .52, .80, .76, .045)


def glyph_mouse(u, v):
    if not _rr(u, v, .33, .16, .67, .84, .17):
        return False
    if _line(u, v, .33, .445, .67, .445, .012): return False
    if _line(u, v, .5, .17, .5, .23, .012): return False
    return not _rr(u, v, .465, .26, .535, .37, .03)


def glyph_keyboard(u, v):
    if not _rr(u, v, .14, .30, .86, .70, .05):
        return False
    for i in range(6):
        if _rr(u, v, .21 + .1 * i, .355, .27 + .1 * i, .415, .012): return False
    for i in range(5):
        if _rr(u, v, .26 + .1 * i, .455, .32 + .1 * i, .515, .012): return False
    return not _rr(u, v, .32, .56, .68, .62, .012)


def glyph_pad(u, v):
    if not _rr(u, v, .14, .33, .86, .69, .13):
        return False
    if _line(u, v, .24, .505, .38, .505, .024): return False
    if _line(u, v, .31, .43, .31, .58, .024): return False
    return not (_circle(u, v, .64, .57, .04) or _circle(u, v, .74, .47, .04))


def glyph_other(u, v):
    return any(_line(u, v, *a, *b, .04) for a, b in RUNE)


GLYPHS = {'spk': glyph_speaker, 'hdst': glyph_headset, 'mouse': glyph_mouse,
          'kbd': glyph_keyboard, 'pad': glyph_pad, 'other': glyph_other}


def device_icon(kind, colour, size=40):
    """A round badge with a white glyph, anti-aliased, as (r, g, b, a) rows."""
    glyph = GLYPHS[kind]
    n = 4
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            inside = white = 0
            for sy in range(n):
                for sx in range(n):
                    u = (x + (sx + .5) / n) / size
                    v = (y + (sy + .5) / n) / size
                    if _circle(u, v, .5, .5, .485):
                        inside += 1
                        if glyph(u, v):
                            white += 1
            if not inside:
                row.append((0, 0, 0, 0))
                continue
            t = (y + .5) / size
            shade = 1.14 - .28 * t
            base = [min(255, int(c * shade)) for c in colour]
            w = white / inside
            rgb = tuple(int(round(base[i] * (1 - w) + 255 * w)) for i in range(3))
            row.append(rgb + (int(round(255 * inside / (n * n))),))
        rows.append(row)
    return rows


def squash_alpha(rows):
    out = []
    for i in range(0, len(rows) - 1, 2):
        a, b = rows[i], rows[i + 1]
        out.append([tuple((a[x][k] + b[x][k]) // 2 for k in range(4)) for x in range(len(a[0]))])
    return out


def device_sprites(ydpi):
    out = []
    for kind in GLYPHS:
        for tag, colour in (('b', ACTIVE), ('g', INACTIVE)):
            rows = device_icon(kind, colour)
            if ydpi != 90:
                rows = squash_alpha(rows)
            out.append(sprite_alpha('dv_%s_%s' % (kind, tag), rows, ydpi))
    return out


def sprite_file(path, sprites):
    body = b''.join(sprites)
    # A sprite area without its first (size) word.
    data = struct.pack('<3I', len(sprites), 16, 16 + len(body)) + body
    with open(path, 'wb') as handle:
        handle.write(data)


def build(out):
    app = os.path.join(out, '!Bluetooth')
    setup = os.path.join(out, '!BTSetup')
    os.makedirs(app, exist_ok=True)
    os.makedirs(setup, exist_ok=True)
    piece = configure_icon(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        'co_piece_shield.rgba'))
    for suffix, ydpi in (('22', 90), ('', 45)):
        def logo(size, colour, small=False):
            rows = render(size, size, colour, small)
            return rows if ydpi == 90 else squash(rows)
        sprite_file(os.path.join(app, '!Sprites%s,ff9' % suffix), [
            sprite('!bluetooth', logo(34, BLUE), ydpi),
            sprite('sm!bluetooth', logo(18, BLUE, True), ydpi),
            sprite('bt_idle', logo(34, GREY), ydpi),
            sprite('bt_busy', logo(34, AMBER), ydpi),
            sprite('bt_on', logo(34, BLUE), ydpi),
        ] + device_sprites(ydpi))
        sprite_file(os.path.join(setup, '!Sprites%s,ff9' % suffix), [
            sprite('!btsetup', logo(34, BLUE), ydpi),
            sprite('sm!btsetup', logo(18, BLUE, True), ydpi),
        ])
        sprite_file(os.path.join(setup, 'CoSprite%s,ff9' % suffix), [
            sprite_alpha('co_bluetooth', piece),
        ])


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else '.')
