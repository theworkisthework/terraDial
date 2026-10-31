"""Build terrapen.wad: a tiny Doom IWAD whose every lump is drawn here, in
code, in the terraPen look -- hatching, stippling, contour lines and ring
dots in black, lime and coral, as if the level were a pen plot.

Nothing in it comes from id's games. The engine (lib/doomgeneric) only
loads what a level actually uses, and our port fills any other graphic it
asks for with TDBLANK, an empty patch -- so this only has to draw the
essentials: palette and light tables, textures, the weapon, and a level.

Pure Python, no packages. Run it directly or let the doom env's pre-script
do it:  python tools/doom/make_wad.py doom/terrapen.wad
"""
import math
import random
import struct
import sys

# ------------------------------------------------------------------ palette
#
# 8 ramps of 32: each runs from near-black up to its colour (24 steps) and
# on towards white (8). Lighting (COLORMAP) darkens by moving down a ramp,
# so every hue has its own shadows.

HUES = [
    ("grey", (235, 235, 235)),
    ("lime", (0xE5, 0xF1, 0x5C)),
    ("coral", (0xE8, 0x3B, 0x68)),
    ("teal", (0x66, 0xCC, 0xCC)),
    ("green", (0x3A, 0x7A, 0x45)),
    ("paper", (0xF6, 0xF1, 0xE4)),
    ("ink", (0x2B, 0x3A, 0x8F)),
    ("plum", (0x7A, 0x3B, 0x8F)),
]
RAMP = 32


def ramp_colour(rgb, i):
    if i < 24:
        t = i / 23.0
        return tuple(int(round(c * t)) for c in rgb)
    t = (i - 23) / 9.0
    return tuple(int(round(c + (255 - c) * t)) for c in rgb)


BASE = [ramp_colour(rgb, i) for _, rgb in HUES for i in range(RAMP)]


def idx(hue, shade):
    """Palette index of `shade` (0 dark .. 31 light) along `hue`'s ramp."""
    h = [n for n, _ in HUES].index(hue)
    return h * RAMP + max(0, min(RAMP - 1, shade))


def nearest(rgb, among=None):
    best, bestd = 0, 1 << 30
    for i in among if among is not None else range(256):
        c = BASE[i]
        d = (c[0] - rgb[0]) ** 2 + (c[1] - rgb[1]) ** 2 + (c[2] - rgb[2]) ** 2
        if d < bestd:
            best, bestd = i, d
    return best


def playpal():
    """14 palettes: normal, 8 hurt (redder), 4 pickup (golder), radsuit (greener)."""
    out = bytearray()

    def tinted(tint, amount):
        for r, g, b in BASE:
            out.extend(int(c + (t - c) * amount) for c, t in zip((r, g, b), tint))

    tinted((0, 0, 0), 0)
    for i in range(1, 9):
        tinted((255, 0, 0), i / 9.0)
    for i in range(1, 5):
        tinted((215, 186, 69), i * 0.125)
    tinted((0, 256, 0), 0.125)
    return bytes(out)


def colormap():
    """34 maps: 32 light levels, the invulnerability greys, and all-black."""
    out = bytearray()
    for level in range(32):
        k = (32 - level) / 32.0
        for r, g, b in BASE:
            out.append(nearest((r * k, g * k, b * k)))
    greys = range(0, RAMP)
    for r, g, b in BASE:
        lum = 255 - (r * 0.3 + g * 0.59 + b * 0.11)
        out.append(nearest((lum, lum, lum), greys))
    out.extend(bytes(256))
    return bytes(out)


# ------------------------------------------------------------------ drawing
#
# A canvas is a list of rows of palette indices, None for transparent.

class Canvas:
    def __init__(self, w, h, fill=None):
        self.w, self.h = w, h
        self.px = [[fill] * w for _ in range(h)]

    def put(self, x, y, c, wrap=False):
        x, y = int(round(x)), int(round(y))
        if wrap:
            x %= self.w
            y %= self.h
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def line(self, x0, y0, x1, y1, c, width=1, wrap=False, wobble=0.0, seed=0):
        """A pen line: a little wobble along it, `width` pixels thick."""
        rnd = random.Random(seed)
        phase, freq = rnd.uniform(0, 6.3), rnd.uniform(0.05, 0.12)
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        dx, dy = x1 - x0, y1 - y0
        length = math.hypot(dx, dy) or 1
        nx, ny = -dy / length, dx / length
        for i in range(n + 1):
            t = i / max(n, 1)
            w = wobble * math.sin(phase + i * freq)
            x, y = x0 + dx * t + nx * w, y0 + dy * t + ny * w
            for o in range(width):
                off = o - (width - 1) / 2.0
                self.put(x + nx * off, y + ny * off, c, wrap)

    def circle(self, cx, cy, r, c, wrap=False, fill=None):
        if fill is not None:
            for y in range(int(cy - r), int(cy + r) + 1):
                for x in range(int(cx - r), int(cx + r) + 1):
                    if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                        self.put(x, y, fill, wrap)
        steps = max(24, int(r * 12))
        for i in range(steps):
            a = 2 * math.pi * i / steps
            self.put(cx + r * math.cos(a), cy + r * math.sin(a), c, wrap)

    def polygon(self, pts, fill):
        """Filled polygon (even-odd scanline)."""
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            for (xa, ya), (xb, yb) in zip(pts, pts[1:] + pts[:1]):
                if (ya <= y < yb) or (yb <= y < ya):
                    xs.append(xa + (y - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for a, b in zip(xs[::2], xs[1::2]):
                for x in range(int(round(a)), int(round(b))):
                    self.put(x, y, fill)

    def outline(self, pts, c, width=1):
        for a, b in zip(pts, pts[1:] + pts[:1]):
            self.line(a[0], a[1], b[0], b[1], c, width)


def hatch(cv, angle_deg, spacing, c, width=1, wobble=0.6, seed=1):
    """Parallel pen strokes across the whole canvas, tiling seamlessly."""
    a = math.radians(angle_deg)
    dx, dy = math.cos(a), math.sin(a)
    span = cv.w + cv.h
    k = -span
    s = seed
    while k < span:
        # A line through (k*nx, k*ny) along (dx, dy), drawn long and wrapped.
        ox, oy = -dy * k, dx * k
        cv.line(ox - dx * span, oy - dy * span, ox + dx * span, oy + dy * span, c, width,
                wrap=True, wobble=wobble, seed=s)
        k += spacing
        s += 1


# ------------------------------------------------------------------ lump formats

def patch(cv, left=0, top=0):
    """Doom picture format: column offsets, then posts of opaque runs."""
    cols = []
    for x in range(cv.w):
        col = bytearray()
        y = 0
        while y < cv.h:
            if cv.px[y][x] is None:
                y += 1
                continue
            start = y
            run = []
            while y < cv.h and cv.px[y][x] is not None and len(run) < 128:
                run.append(cv.px[y][x])
                y += 1
            col += bytes([start, len(run), run[0]]) + bytes(run) + bytes([run[-1]])
        col.append(0xFF)
        cols.append(bytes(col))
    head = struct.pack("<hhhh", cv.w, cv.h, left, top)
    offs = []
    pos = len(head) + 4 * cv.w
    for col in cols:
        offs.append(pos)
        pos += len(col)
    return head + struct.pack("<%dI" % cv.w, *offs) + b"".join(cols)


def flat(cv):
    assert cv.w == 64 and cv.h == 64
    return bytes(c or 0 for row in cv.px for c in row)


def name8(s):
    return s.encode("ascii").ljust(8, b"\0")[:8]


# ------------------------------------------------------------------ textures

def wall_hatch():
    """Lime cross-hatching on near-black, a coral rule along the top."""
    cv = Canvas(128, 128, idx("grey", 3))
    hatch(cv, 45, 9, idx("lime", 20), wobble=0.4, seed=10)
    hatch(cv, -45, 16, idx("lime", 12), wobble=0.4, seed=40)
    for y in (2, 5):
        cv.line(0, y, 127, y, idx("coral", 22), wrap=True, wobble=0.4, seed=y)
    return cv


def wall_stipple():
    """Coral stipple thinning out downwards, over a teal baseline."""
    cv = Canvas(128, 128, idx("grey", 2))
    rnd = random.Random(7)
    for _ in range(2600):
        x, y = rnd.randrange(128), rnd.randrange(128)
        if rnd.random() < 1.0 - y / 150.0:
            cv.put(x, y, idx("coral", rnd.randint(17, 23)))
    for y in range(118, 128, 3):
        cv.line(0, y, 127, y, idx("teal", 20), wrap=True, wobble=0.5, seed=y)
    return cv


def floor_rings():
    """The playbook's pattern: rings and dots, coral on black."""
    cv = Canvas(64, 64, idx("grey", 2))
    for gx in range(4):
        for gy in range(4):
            cx, cy = gx * 16 + (8 if gy % 2 else 0), gy * 16 + 8
            big = (gx + gy) % 3 == 0
            cv.circle(cx, cy, 5 if big else 3, idx("coral", 20), wrap=True)
            if big:
                cv.circle(cx, cy, 2, idx("coral", 20), wrap=True)
    return cv


def ceil_contours():
    """Contour lines, like a plotted height map."""
    cv = Canvas(64, 64, idx("grey", 2))
    for y in range(64):
        for x in range(64):
            v = (math.sin(x * 2 * math.pi / 64) + math.cos(y * 2 * math.pi / 64) +
                 0.5 * math.sin((x + y) * 4 * math.pi / 64))
            if abs((v * 3) % 1.0 - 0.5) < 0.09:
                cv.put(x, y, idx("teal", 16))
    return cv


def sky():
    """Lime paper with a coral horizon, hatched in bands."""
    cv = Canvas(256, 128, idx("lime", 22))
    for i, y in enumerate(range(70, 128, 4)):
        cv.line(0, y, 255, y, idx("coral", 18 + (i % 3)), wrap=True, wobble=1.2, seed=100 + i)
    for x in range(0, 256, 32):
        cv.circle(x + 16, 30, 6, idx("coral", 21), wrap=True)
    return cv


def sky_flat():
    return Canvas(64, 64, idx("lime", 22))


# ------------------------------------------------------------------ sprites
#
# The weapon is a pen, held from the bottom right with the nib at the
# crosshair. Weapon sprites sit in screen space, hung from the middle of the
# view: a left offset of -x puts the patch's left edge at screen x + 1, and
# a top offset of t puts its top at CENTERY - 68 - t. The Knob renders a
# 320x254 frame (src/doom/doom_main.cpp), so the view's middle is row 127.

VIEW_H = 254
CENTERY = VIEW_H // 2


def top_offset(screen_y):
    """The top offset that puts a weapon patch's top row at screen_y."""
    return CENTERY - 68 - screen_y

def pen(cv, tip_x, tip_y, ink=False):
    body = idx("coral", 22)
    band = idx("lime", 22)
    edge = idx("grey", 1)
    metal = idx("grey", 26)
    # The barrel: a fat diagonal from off the bottom-right corner to the nib.
    ax, ay = tip_x + 34, tip_y + 30
    bx, by = cv.w + 40, cv.h + 40
    ux, uy = bx - ax, by - ay
    ln = math.hypot(ux, uy)
    nx, ny = -uy / ln * 13, ux / ln * 13
    barrel = [(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)]
    cv.polygon(barrel, body)
    cv.outline(barrel, edge, 2)
    t = 0.18
    bx0, by0 = ax + ux * t, ay + uy * t
    bx1, by1 = ax + ux * (t + 0.08), ay + uy * (t + 0.08)
    stripe = [(bx0 + nx, by0 + ny), (bx1 + nx, by1 + ny), (bx1 - nx, by1 - ny), (bx0 - nx, by0 - ny)]
    cv.polygon(stripe, band)
    cv.outline(stripe, edge, 1)
    # The nib: a steel wedge with a slit and a breather hole.
    nib = [(tip_x, tip_y), (ax + nx * 0.8, ay + ny * 0.8), (ax - nx * 0.8, ay - ny * 0.8)]
    cv.polygon(nib, metal)
    cv.outline(nib, edge, 2)
    cv.line(tip_x, tip_y, (ax + tip_x) / 2 + 2, (ay + tip_y) / 2 + 2, edge, 1)
    cv.circle((ax + tip_x) / 2 + 3, (ay + tip_y) / 2 + 3, 1.5, edge)
    if ink:
        cv.circle(tip_x - 2, tip_y - 2, 3, edge, fill=idx("ink", 18))


def weapon_patch(dx=0, dy=0, ink=False):
    w, h = 150, 170            # tall enough to run off the bottom of the view
    cv = Canvas(w, h)
    pen(cv, 18 + dx, 14 + dy, ink)
    left_x = 160 - 18          # tip lands on the centre column...
    top = CENTERY - 14         # ...and the middle row
    return patch(cv, -left_x, top_offset(top))


def flash_patch():
    cv = Canvas(40, 40)
    rnd = random.Random(3)
    cv.circle(20, 20, 9, idx("ink", 16), fill=idx("ink", 20))
    for _ in range(14):
        a, r = rnd.uniform(0, 6.3), rnd.uniform(11, 17)
        cv.circle(20 + r * math.cos(a), 20 + r * math.sin(a), rnd.uniform(1, 2.5),
                  idx("ink", 16), fill=idx("ink", 22))
    # Centred on the nib, at the middle of the view.
    return patch(cv, -(161 - 20 - 1), top_offset(CENTERY - 20))


def blot(r, hue, seed, fade=0):
    size = int(r * 2 + 6)
    cv = Canvas(size, size)
    rnd = random.Random(seed)
    c = size / 2
    cv.circle(c, c, r, idx(hue, 14 - fade), fill=idx(hue, 20 - fade))
    for _ in range(5):
        a = rnd.uniform(0, 6.3)
        cv.circle(c + (r + 1.5) * math.cos(a), c + (r + 1.5) * math.sin(a), 1, idx(hue, 18 - fade),
                  fill=idx(hue, 18 - fade))
    return patch(cv, size // 2, size)


# ------------------------------------------------------------------ status bar

SEGS = {  # seven-segment digits: a b c d e f g
    "0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc",
    "5": "afgcd", "6": "afgedc", "7": "abc", "8": "abcdefg", "9": "abcdfg",
    "-": "g",
}


def digit(ch, w, h, colour):
    cv = Canvas(w, h)
    m, t = 1, max(1, w // 6)
    mid = h // 2
    seg = {
        "a": ((m, m), (w - m - 1, m)), "d": ((m, h - m - 1), (w - m - 1, h - m - 1)),
        "g": ((m, mid), (w - m - 1, mid)), "f": ((m, m), (m, mid)), "b": ((w - m - 1, m), (w - m - 1, mid)),
        "e": ((m, mid), (m, h - m - 1)), "c": ((w - m - 1, mid), (w - m - 1, h - m - 1)),
    }
    for s in SEGS[ch]:
        (x0, y0), (x1, y1) = seg[s]
        cv.line(x0, y0, x1, y1, colour, t)
    return cv


def percent(w, h, colour):
    cv = Canvas(w, h)
    cv.line(w - 2, 1, 1, h - 2, colour, max(1, w // 6))
    cv.circle(w * 0.28, h * 0.22, w * 0.15, colour)
    cv.circle(w * 0.72, h * 0.78, w * 0.15, colour)
    return cv


def stbar():
    cv = Canvas(320, 32, idx("grey", 2))
    cv.line(0, 0, 319, 0, idx("lime", 22), 2)
    for x in range(0, 320, 6):
        cv.line(x, 31, x + 6, 26, idx("grey", 6))
    return cv


# ------------------------------------------------------------------ the level

def octagon_room(radius=384, sides=8):
    """MAP01, for a first look: one convex room, so one subsector and no
    BSP to build. A real node builder comes with real levels."""
    verts = []
    for i in range(sides):  # clockwise, so each wall's right side faces in
        a = -2 * math.pi * i / sides + math.pi / sides
        verts.append((int(radius * math.cos(a)), int(radius * math.sin(a))))
    lines = [(i, (i + 1) % sides) for i in range(sides)]
    textures = ["WALL1" if i % 2 == 0 else "WALL2" for i in range(sides)]

    things = struct.pack("<hhhhh", 0, -radius // 2, 90, 1, 7)  # player 1 start

    linedefs = b"".join(struct.pack("<hhhhhhh", a, b, 1, 0, 0, i, -1) for i, (a, b) in enumerate(lines))
    sidedefs = b"".join(struct.pack("<hh8s8s8sh", 0, 0, name8("-"), name8("-"), name8(t), 0) for t in textures)
    vertexes = b"".join(struct.pack("<hh", x, y) for x, y in verts)
    segs = bytearray()
    for i, (a, b) in enumerate(lines):
        (x0, y0), (x1, y1) = verts[a], verts[b]
        bam = int(round(math.atan2(y1 - y0, x1 - x0) / (2 * math.pi) * 65536)) & 0xFFFF
        segs += struct.pack("<hhhhhh", a, b, bam - 65536 if bam > 32767 else bam, i, 0, 0)
    ssectors = struct.pack("<hh", len(lines), 0)
    # Tall and fully lit: in a low, dim room the far walls shrank to thin
    # strips and faded to black.
    sectors = struct.pack("<hh8s8shhh", 0, 256, name8("TDFLOOR"), name8("TDCEIL"), 255, 0, 0)
    reject = bytes(1)
    blockmap = make_blockmap(verts, lines)
    return [("MAP01", b""), ("THINGS", things), ("LINEDEFS", linedefs), ("SIDEDEFS", sidedefs),
            ("VERTEXES", vertexes), ("SEGS", bytes(segs)), ("SSECTORS", ssectors), ("NODES", b""),
            ("SECTORS", sectors), ("REJECT", reject), ("BLOCKMAP", blockmap)]


def make_blockmap(verts, lines, block=128):
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    ox, oy = min(xs) - 8, min(ys) - 8
    cols = (max(xs) - ox) // block + 1
    rows = (max(ys) - oy) // block + 1
    lists = []
    for r in range(rows):
        for c in range(cols):
            bx0, by0 = ox + c * block, oy + r * block
            bx1, by1 = bx0 + block, by0 + block
            hit = []
            for i, (a, b) in enumerate(lines):
                (x0, y0), (x1, y1) = verts[a], verts[b]
                if max(x0, x1) >= bx0 and min(x0, x1) <= bx1 and max(y0, y1) >= by0 and min(y0, y1) <= by1:
                    hit.append(i)
            lists.append(hit)
    header = 4 + cols * rows
    out = [struct.pack("<hhhh", ox, oy, cols, rows)]
    offs, body = [], []
    pos = header
    for hit in lists:
        offs.append(pos)
        entry = [0] + hit + [-1]
        body.append(struct.pack("<%dh" % len(entry), *entry))
        pos += len(entry)
    out.append(struct.pack("<%dH" % len(offs), *offs))
    return b"".join(out + body)


# ------------------------------------------------------------------ assembly

def texture_lumps(textures):
    """PNAMES + TEXTURE1 for single-patch textures: [(name, patchname, w, h)]."""
    pnames = [t[1] for t in textures]
    pn = struct.pack("<i", len(pnames)) + b"".join(name8(p) for p in pnames)
    entries = [name8(n) + struct.pack("<ihhihhhhhh", 0, w, h, 0, 1, 0, 0, i, 1, 0)
               for i, (n, _, w, h) in enumerate(textures)]
    head = 4 + 4 * len(entries)
    offs, pos = [], head
    for e in entries:
        offs.append(pos)
        pos += len(e)
    t1 = struct.pack("<i", len(entries)) + struct.pack("<%di" % len(offs), *offs) + b"".join(entries)
    return pn, t1


def build():
    lumps = [("PLAYPAL", playpal()), ("COLORMAP", colormap()), ("TDBLANK", patch(Canvas(1, 1)))]

    lumps.append(("STBAR", patch(stbar())))
    for ch in "0123456789":
        lumps.append(("STTNUM" + ch, patch(digit(ch, 14, 16, idx("lime", 22)))))
        lumps.append(("STYSNUM" + ch, patch(digit(ch, 4, 6, idx("coral", 22)))))
        lumps.append(("STGNUM" + ch, patch(digit(ch, 4, 6, idx("grey", 20)))))
    lumps.append(("STTMINUS", patch(digit("-", 14, 16, idx("lime", 22)))))
    lumps.append(("STTPRCNT", patch(percent(14, 16, idx("lime", 22)))))

    # Texture 0 means "no texture" to the engine and is never drawn, so
    # the list starts with a placeholder.
    pn, t1 = texture_lumps([("TDNULL", "TDWALL1", 128, 128),
                            ("WALL1", "TDWALL1", 128, 128), ("WALL2", "TDWALL2", 128, 128),
                            ("SKY1", "TDSKY", 256, 128)])
    lumps += [("PNAMES", pn), ("TEXTURE1", t1)]
    lumps += octagon_room()

    lumps.append(("S_START", b""))
    lumps += [("PISGA0", weapon_patch()), ("PISGB0", weapon_patch(-6, -8, ink=True)),
              ("PISGC0", weapon_patch(4, 6)), ("PISFA0", flash_patch())]
    lumps += [("PUNGA0", weapon_patch(10, 20)), ("PUNGB0", weapon_patch(0, 6)),
              ("PUNGC0", weapon_patch(-10, -6)), ("PUNGD0", weapon_patch(-14, -10))]
    for i, f in enumerate("ABCD"):
        lumps.append(("PUFF%s0" % f, blot(2 + i, "ink", 20 + i, fade=i * 3)))
    for i, f in enumerate("ABC"):
        lumps.append(("BLUD%s0" % f, blot(2 + i, "coral", 30 + i)))
    lumps.append(("S_END", b""))

    lumps += [("P_START", b""), ("TDWALL1", patch(wall_hatch())), ("TDWALL2", patch(wall_stipple())),
              ("TDSKY", patch(sky())), ("P_END", b"")]
    lumps += [("F_START", b""), ("TDFLOOR", flat(floor_rings())), ("TDCEIL", flat(ceil_contours())),
              ("F_SKY1", flat(sky_flat())), ("F_END", b"")]
    return lumps


def write_wad(path, lumps):
    data = bytearray()
    directory = []
    pos = 12
    for name, body in lumps:
        directory.append(struct.pack("<ii8s", pos if body else 0, len(body), name8(name)))
        data += body
        pos += len(body)
    with open(path, "wb") as f:
        f.write(b"IWAD" + struct.pack("<ii", len(lumps), 12 + len(data)))
        f.write(data)
        f.write(b"".join(directory))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "doom/terrapen.wad"
    lumps = build()
    write_wad(out, lumps)
    print("wrote %s: %d lumps" % (out, len(lumps)))
