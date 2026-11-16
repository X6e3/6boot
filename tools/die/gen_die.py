# SPDX-License-Identifier: GPL-2.0-or-later
"""The 6boot die: a floorplan of 6boot's own stage2, one block per part, each block's area its
non-blank source lines (P2T stage2 + P3 entry + boot trace, counted 21 Oct 2026).

Hue says who owns the code and what it is: m1n1 and its libraries in bronze, 6boot's own parts in
olive, one chip with two hands; within each family a tone per kind of block, as on a die shot
(logic, SRAM, PHYs, cores, buses, mixed). Brightness says power: a block lights when its boot step
runs and goes dark again when 6boot tears it down. The hypervisor is built in but never runs during
boot; its block is the TM block instead, lit throughout: an ice-blue to cold-navy square wave from
a framed square, the one colour on the die that is not metal (6 Nov 2026, the first ANE matmul on
Linux).

    gen_die.py <outdir>     writes die-<state>.png previews and blocks.json
"""
import json
import random
import struct
import sys
import zlib

out = sys.argv[1]

# name, lines, owner, step that powers it on (-1: on before step 0), step that powers it off
BLOCKS = [
    # 6boot
    ('boot trace',     409, '6boot', 0,  14),
    ('hooks',          242, '6boot', 0,  14),
    ('contract',       246, '6boot', 1,  14),
    ('adapter',        328, '6boot', 3,  14),
    ('entry probe',    279, '6boot', 3,  14),
    ('handoff record',  54, '6boot', 3,  14),
    # m1n1 and its libraries
    ('heap',          5634, 'm1n1', -1, 14),
    ('runtime',       3027, 'm1n1', -1, 14),
    ('memory',        2321, 'm1n1', -1, 12),
    ('exceptions',     636, 'm1n1', -1, 8),
    ('display',       1060, 'm1n1', -1, 10),
    ('framebuffer',    484, 'm1n1', -1, 11),
    ('coprocessors',  1752, 'm1n1', -1, 10),
    ('NVMe',           487, 'm1n1', -1, 8),
    ('payload',       3093, 'm1n1', 0,  14),
    ('CPUs',          2140, 'm1n1', 1,  14),
    ('kboot',         3908, 'm1n1', 3,  14),
    ('libfdt',        2728, 'm1n1', 3,  14),
    ('power',         1687, 'm1n1', 4,  14),
    ('USB',           2425, 'm1n1', 5,  9),
    ('PCIe',           763, 'm1n1', 6,  14),
    ('hypervisor',    3283, 'm1n1', -1, 127),  # the TM block: lit from the start, never torn down
]
KIND = {'heap': 'sram', 'memory': 'sram', 'runtime': 'logic', 'kboot': 'logic', 'libfdt': 'logic',
        'payload': 'logic', 'hypervisor': 'tm', 'USB': 'phy', 'PCIe': 'phy', 'NVMe': 'phy',
        'display': 'mixed', 'coprocessors': 'mixed', 'framebuffer': 'sram', 'CPUs': 'cores',
        'exceptions': 'logic', 'power': 'bus', 'boot trace': 'sram', 'hooks': 'logic',
        'contract': 'logic', 'adapter': 'mixed', 'entry probe': 'cores', 'handoff record': 'bus'}


def squarify(items, x, y, w, h):
    """Squarified treemap (Bruls, Huizing, van Wijk). items: [(key, area)], areas sum to w*h."""
    rects, items = [], sorted(items, key=lambda i: -i[1])

    def worst(row, side):
        s = sum(a for _, a in row)
        return max(max(side * side * a / (s * s), s * s / (side * side * a)) for _, a in row)

    while items:
        side = min(w, h)
        row = [items.pop(0)]
        while items and worst(row + [items[0]], side) <= worst(row, side):
            row.append(items.pop(0))
        s = sum(a for _, a in row)
        if w >= h:
            cw, cy = s / h, y
            for k, a in row:
                rects.append((k, x, cy, cw, a / cw))
                cy += a / cw
            x, w = x + cw, w - cw
        else:
            ch, cx = s / w, x
            for k, a in row:
                rects.append((k, cx, y, a / ch, ch))
                cx += a / ch
            y, h = y + ch, h - ch
    return rects


def hexc(s):
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


# Hue: m1n1's family is bronze, 6boot's olive, each with a tone per kind of block. Brightness: power.
# Amber edge: the block's step is running.
KINDPAL = {
    'logic': [hexc(c) for c in ('b89a52', '8a7a3a', '5a5228', 'd8c27a', '6a6a8a', '8a4a3a', '3a3420')],
    'sram':  [hexc(c) for c in ('c4b88c', '948a60', '5c5434', 'e6dcb4', '7c7a74', '8a6c40', '2a2618')],
    'phy':   [hexc(c) for c in ('c08a52', '8c5e34', '5a3c22', 'e2b484', '7a6a66', '9a4c30', '2a1a10')],
    'mixed': [hexc(c) for c in ('b49a64', '86703e', '564628', 'd8c290', '74706a', '8a5a3a', '2a2216')],
    'cores': [hexc(c) for c in ('d0b054', 'a08832', '6a5a22', 'f0d894', '8a7a5c', 'a06a32', '30280c')],
    'bus':   [hexc(c) for c in ('a8865a', '765e3a', '4c3c24', 'ccac80', '6c6a6a', '7c4c2c', '221a10')],
}
KINDPAL_O = {
    'logic': [hexc(c) for c in ('b8c860', '8a9a40', '545c28', 'e0e8a0', '7a8a6a', '8a6a3a', '2a3018')],
    'sram':  [hexc(c) for c in ('c8d08c', '9aa264', '5c6234', 'e6ecbc', '7c8274', '8a7044', '2a2e18')],
    'mixed': [hexc(c) for c in ('aab460', '7c843a', '4c5226', 'd4d89a', '6c7a6c', '8a6a3c', '262a18')],
    'cores': [hexc(c) for c in ('c8c850', '9a9a30', '625e20', 'eceaa0', '8a8a5c', '9a7034', '2e2c0c')],
    'bus':   [hexc(c) for c in ('98a058', '6e7438', '464a24', 'c0c888', '6a6e66', '7a5a30', '20240e')],
    'phy':   [hexc(c) for c in ('a8b46c', '7a8444', '4a5228', 'd0d8a0', '6c766c', '8a6a3c', '242a16')],
}
# The TM block: standard cells in ice blue near the square, cold navy at the far corners
ICE, NAVY, ICE_EDGE = hexc('2ee6ff'), hexc('1a2a78'), hexc('d8fbff')
SUBSTRATE, RING, PAD, AMBER = hexc('16140c'), hexc('c8b04a'), hexc('9a8a4a'), hexc('ffa01e')


def layout(size):
    edge = size // 32                      # seal ring + pad ring
    inner = size - 2 * edge
    own = [(b[0], b[1]) for b in BLOCKS if b[2] == '6boot']
    rest = sorted([(b[0], b[1]) for b in BLOCKS if b[2] != '6boot'], key=lambda i: -i[1])
    total = sum(b[1] for b in BLOCKS)
    s = round(inner * (sum(a for _, a in own) / total) ** 0.5)
    # 6boot in the top-left corner; m1n1 fills the L around it, split into a right and a bottom part
    right, bottom = (edge + s, edge, inner - s, inner), (edge, edge + s, s, inner - s)
    cap = [right[2] * right[3], bottom[2] * bottom[3]]
    parts, fill = [[], []], [0, 0]
    for k, a in rest:
        i = 0 if fill[0] / cap[0] <= fill[1] / cap[1] else 1
        parts[i].append((k, a))
        fill[i] += a
    rects = {}
    for reg, items in ((right, parts[0]), (bottom, parts[1]), ((edge, edge, s, s), own)):
        x, y, w, h = reg
        tot = sum(a for _, a in items)
        for k, rx, ry, rw, rh in squarify([(k, a * w * h / tot) for k, a in items], x, y, w, h):
            rects[k] = (round(rx), round(ry), round(rx + rw) - round(rx), round(ry + rh) - round(ry))
    return edge, rects


def ramp(b):
    """A block palette from one colour: base, shades, tint, as the tones of the other blocks."""
    sc = lambda f: tuple(min(255, int(v * f)) for v in b)
    return [b, sc(.72), sc(.42), tuple(min(255, v + (255 - v) * 55 // 100) for v in b), sc(.6), sc(.3), sc(.15)]


def tm_wave(w, h, rng):
    """The TM block: the same standard cells in ice and in navy, blended by Chebyshev distance from a
    framed square (half-side 6), with a faint ring every 6 px."""
    state = rng.getstate()
    a = texture('logic', w, h, ramp(ICE), rng)
    rng.setstate(state)                      # the same cells, in the other colour
    b = texture('logic', w, h, ramp(NAVY), rng)
    smooth = lambda t: t * t * (3 - 2 * t)
    core, ring = 6, 6
    cx, cy = round(w * 0.34), round(h * 0.36)
    reach = max(cx, w - 1 - cx, cy, h - 1 - cy)
    px = []
    for j in range(h):
        for i in range(w):
            d, p, q = max(abs(i - cx), abs(j - cy)), a[j * w + i], b[j * w + i]
            if d <= core:
                c = ICE_EDGE if d == core else tuple(min(255, v + 40) for v in p)
            else:
                t = smooth(min(1.0, (d - core) / (reach - core)))
                c = tuple(round(x * (1 - t) + y * t) for x, y in zip(p, q))
                if (d - core) % ring == 0 and t < 0.9:
                    c = tuple(min(255, v + round(70 * (1 - t))) for v in c)
            px.append(c)
    return px


def texture(kind, w, h, pal, rng):
    """Die-photo textures: standard-cell rows, SRAM banks, PHY pads, cores, buses; and the TM wave."""
    if kind == 'tm':
        return tm_wave(w, h, rng)
    px = [SUBSTRATE] * (w * h)
    put = lambda x, y, c: px.__setitem__(y * w + x, c)
    if kind in ('logic', 'mixed', 'cores'):
        top = h // 2 if kind == 'mixed' else 0
        if kind == 'mixed':                  # an SRAM macro over the logic
            for y in range(0, top):
                for x in range(w):
                    put(x, y, pal[3] if (x % 3 == 0 or y % 3 == 0) else pal[1] if (x // 24 + y // 24) % 2 else pal[0])
        row = 5
        for y0 in range(top, h, row):        # standard-cell rows, cells of random width and tone
            x = 0
            while x < w:
                cw, c = rng.randint(2, 7), rng.choice(pal[:3])
                for y in range(y0, min(y0 + row - 1, h)):
                    for xx in range(x, min(x + cw, w)):
                        put(xx, y, c if (xx - x) else pal[2])
                x += cw
        if kind == 'cores':                  # repeated core tiles, each with its cache
            tile = 24
            for ty in range(0, h - tile + 1, tile):
                for tx in range(0, w - tile + 1, tile):
                    for y in range(ty + 2, ty + 11):
                        for x in range(tx + 2, tx + tile - 2):
                            put(x, y, pal[3] if (x + y) % 2 else pal[0])
                    for x in range(tx, tx + tile):
                        put(x, ty, pal[6])
    elif kind == 'sram':                     # banks of bit cells, decoders between them
        for y in range(h):
            for x in range(w):
                bank = (x % 40 < 3) or (y % 40 < 3)
                put(x, y, pal[2] if bank else pal[3] if (x % 2 and y % 2) else pal[0] if y % 4 else pal[1])
    elif kind == 'phy':                      # big pads and caps with bright lanes between
        for y in range(h):
            for x in range(w):
                put(x, y, pal[4] if (x % 14 < 10 and y % 14 < 10) else pal[3] if y % 14 == 12 else pal[2])
    else:                                    # bus: parallel wires
        for y in range(h):
            for x in range(w):
                put(x, y, (pal[5], pal[1], pal[4], pal[6])[x % 4] if x % 2 == 0 else pal[6])
    return px


def render(size, step, seed=6, fail=None, all_on=False):
    """The die at `step`: powered blocks bright, others dim, the running step's block edged amber
    (red if it failed)."""
    edge, rects = layout(size)
    rng = random.Random(seed)
    img = [SUBSTRATE] * (size * size)
    lit = [False] * (size * size)
    for name, _, owner, on, off in BLOCKS:
        x, y, w, h = rects[name]
        w, h = max(w - 1, 1), max(h - 1, 1)  # 1 px routing channel
        tex = texture(KIND[name], w, h, (KINDPAL_O if owner == '6boot' else KINDPAL).get(KIND[name]), rng)
        powered = all_on or (on < step < off) or (on == -1 and step < off)
        running = not all_on and step == on >= 0
        mark = (0xff, 0x30, 0x30) if fail == name else AMBER
        for j in range(h):
            for i in range(w):
                c = tex[j * w + i]
                if running and (i < 3 or j < 3 or i >= w - 3 or j >= h - 3):
                    c = mark
                elif not (powered or running):
                    c = tuple(v * 22 // 100 for v in c)
                img[(y + j) * size + x + i] = c
                lit[(y + j) * size + x + i] = powered or running
    for j in range(edge, size - edge):       # metal power straps across the core area
        for i in range(edge, size - edge):
            if (i - edge) % 32 == 0 or (j - edge) % 32 == 0:
                c = img[j * size + i]
                k = 60 if lit[j * size + i] else 18  # noqa: E501
                img[j * size + i] = tuple(min(255, v + k) for v in c)
    for j in range(size):                    # seal ring, then a ring of bond pads
        for i in range(size):
            d = min(i, j, size - 1 - i, size - 1 - j)
            ring, pitch = max(2, size // 170), 2 * edge  # scale with the die: 256 px keeps its pads
            if d < ring:
                img[j * size + i] = RING
            elif d < edge:
                along = j if d in (i, size - 1 - i) else i
                off = edge + ((size - 2 * edge) % pitch + pitch - pitch * 5 // 8) // 2  # whole pads, centred
                pad = ring < d < edge - 1 and off <= along < size - off and (along - off) % pitch < pitch * 5 // 8
                img[j * size + i] = PAD if pad else SUBSTRATE
    return img, rects


def png(path, img, size):
    raw = b''.join(b'\x00' + b''.join(bytes(img[y * size + x]) for x in range(size)) for y in range(size))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def emit_c(outdir):
    """Data for the C side at 256 px: the die with every block powered (die_256.bin, 256x256 RGBA:
    exactly 256K, an SPU's local store), the boot logos m1n1 draws before step 0 (blocks that are
    on from the start lit, the rest dim), and the block table."""
    size = 256
    full, rects = render(size, 0, all_on=True)
    cold, _ = render(size, -1)

    def rgba(img, n):
        return b''.join(bytes(c) + b'\xff' for c in img[:n * n]) if n == size else None

    open(f'{outdir}/die_256.bin', 'wb').write(rgba(full, size))
    open(f'{outdir}/bootlogo_256.bin', 'wb').write(rgba(cold, size))
    for n in (128, 48):  # box filter of the cold die
        out_px = bytearray()
        for y in range(n):
            for x in range(n):
                x0, x1 = x * size // n, (x + 1) * size // n
                y0, y1 = y * size // n, (y + 1) * size // n
                q = [cold[yy * size + xx] for yy in range(y0, y1) for xx in range(x0, x1)]
                out_px += bytes(sum(c[i] for c in q) // len(q) for i in range(3)) + b'\xff'
        open(f'{outdir}/bootlogo_{n}.bin', 'wb').write(out_px)
    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */', '/* Generated by gen_die.py from the 6boot source line counts. Do not edit. */',
             'struct die_block {', '    u16 x, y, w, h;', '    s8 on, off;', '    u8 own;', '};', '',
             f'#define DIE_BLOCKS {len(BLOCKS)}', 'static const struct die_block die_block[DIE_BLOCKS] = {']
    for name, n_lines, owner, on, off in BLOCKS:
        x, y, w, h = rects[name]
        lines.append(f'    {{{x}, {y}, {w}, {h}, {min(on, 127)}, {min(off, 127)}, {int(owner == "6boot")}}}, '
                     f'/* {name}, {n_lines} lines */')
    lines.append('};')
    open(f'{outdir}/die_blocks.h', 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--c':
        emit_c(out)
        sys.exit(0)
    for step, tag, fail in [(-1, 'cold', None), (6, 'pcie', None), (7, 'running', None),
                            (13, 'kernel', None), (6, 'pcie-failed', 'PCIe')]:
        img, rects = render(512, step, fail=fail)
        png(f'{out}/die-{tag}.png', img, 512)
    own = sum(b[1] for b in BLOCKS if b[2] == '6boot')
    total = sum(b[1] for b in BLOCKS)
    json.dump({'lines_6boot': own, 'lines_total': total, 'rects_512': rects,
               'blocks': [dict(zip(('name', 'lines', 'owner', 'on', 'off'), b)) for b in BLOCKS]},
              open(f'{out}/blocks.json', 'w'), indent=1)
    print(f'6boot {own} of {total} lines ({100 * own / total:.1f}%)')
