"""The baddies and pickups for terrapen.wad, drawn in code like everything
else. Each baddie borrows one of the engine's monster slots for its
behaviour, and gets every frame that slot's states use:

  Scribble  -- slot POSS (hovers about, shoots from range): a tangled ball
               of wobbly teal line with two eyes. Flicks ink. Dies by
               unravelling into a loose line on the floor; a big hit
               splatters it.
  Blot      -- slot SARG (charges and bites up close): a fat coral ink
               splat with a jagged mouth. Dies by melting into a puddle.

  ink bottle (CLIP), spare nib (STIM), sheet of card (ARM1).
"""
import math
import random

from make_wad import Canvas, idx, patch


def world_patch(cv, hover=0):
    """A sprite standing on its origin: centred, bottom at the floor."""
    return patch(cv, cv.w // 2, cv.h + hover)


# ------------------------------------------------------------------ Scribble

def tangle(cv, cx, cy, rx, ry, seed, loops=5, colour=None, spread=1.0, squash=1.0):
    """A ball of scribble: a few closed wobbly loops, as a pen would draw."""
    colour = colour if colour is not None else idx("teal", 22)
    rnd = random.Random(seed)
    for _ in range(loops):
        a1, a2, a3 = (rnd.uniform(0, 6.3) for _ in range(3))
        k1, k2 = rnd.choice([2, 3]), rnd.choice([3, 4, 5])
        steps = 120
        prev = None
        for i in range(steps + 1):
            t = 2 * math.pi * i / steps
            r = 0.75 + 0.18 * math.sin(k1 * t + a1) + 0.1 * math.sin(k2 * t + a2) * spread
            x = cx + rx * r * math.cos(t + a3)
            y = cy + ry * r * math.sin(t + a3) * squash
            if prev:
                cv.line(prev[0], prev[1], x, y, colour)
            prev = (x, y)


def eyes(cv, cx, cy, gap, look=0, shut=False, cross=False):
    white, ink = idx("grey", 30), idx("grey", 1)
    for side in (-1, 1):
        ex = cx + side * gap
        if cross:
            cv.line(ex - 2, cy - 2, ex + 2, cy + 2, white, 2)
            cv.line(ex - 2, cy + 2, ex + 2, cy - 2, white, 2)
        elif shut:
            cv.line(ex - 3, cy, ex + 3, cy, white, 2)
        else:
            cv.circle(ex, cy, 3, white, fill=white)
            cv.put(ex + look, cy, ink)
            cv.put(ex + look, cy + 1, ink)


def scribble_frames():
    W, H = 48, 52
    frames = {}
    # A-D: hovering about, the tangle redrawn a little differently each step.
    for i, f in enumerate("ABCD"):
        cv = Canvas(W, H)
        bob = [0, -2, 0, 2][i]
        tangle(cv, W / 2, 24 + bob, 18, 18, seed=100 + i)
        eyes(cv, W / 2, 22 + bob, 6, look=[0, 1, 0, -1][i])
        frames[f] = cv
    # E: taking aim -- squinting. F: the flick of ink.
    cv = Canvas(W, H)
    tangle(cv, W / 2, 24, 18, 18, seed=110)
    eyes(cv, W / 2, 22, 6, shut=True)
    frames["E"] = cv
    cv = Canvas(W, H)
    tangle(cv, W / 2, 24, 19, 17, seed=111)
    eyes(cv, W / 2, 22, 6)
    cv.circle(W / 2, 34, 4, idx("ink", 14), fill=idx("ink", 20))
    frames["F"] = cv
    # G: hurt -- squashed, eyes crossed.
    cv = Canvas(W, H)
    tangle(cv, W / 2, 28, 20, 14, seed=112, squash=0.8)
    eyes(cv, W / 2, 26, 6, cross=True)
    frames["G"] = cv
    # H-L: unravelling -- fewer loops, sinking, until a loose line on the floor.
    for i, f in enumerate("HIJKL"):
        cv = Canvas(W, H)
        t = (i + 1) / 5.0
        loops = max(1, 5 - i)
        tangle(cv, W / 2, 24 + 22 * t, 18 + 4 * t, 18 * (1 - 0.85 * t) + 1, seed=120 + i, loops=loops)
        if i < 2:
            eyes(cv, W / 2, 22 + 22 * t, 6, cross=True)
        frames[f] = cv
    # M-U: a big hit -- it bursts into flying ink and settles into a puddle.
    rnd = random.Random(7)
    for i, f in enumerate("MNOPQRSTU"):
        cv = Canvas(W, H)
        t = i / 8.0
        if i < 5:
            tangle(cv, W / 2, 24 + 10 * t, 18 * (1 - t), 18 * (1 - t), seed=130 + i, loops=max(1, 4 - i))
            for _ in range(10):
                a, r = rnd.uniform(0, 6.3), 6 + 30 * t
                cv.circle(W / 2 + r * math.cos(a), 26 + r * 0.7 * math.sin(a) + 14 * t, 1.5,
                          idx("teal", 18), fill=idx("teal", 22))
        puddle = 6 + 14 * t
        for y in range(H - 6, H):
            half = puddle * math.sqrt(max(0, 1 - ((y - (H - 3)) / 3.5) ** 2))
            cv.line(W / 2 - half, y, W / 2 + half, y, idx("teal", 12))
        frames[f] = cv
    return frames


# ------------------------------------------------------------------ Blot

def blob(cv, cx, cy, r, seed, stretch=1.0, squash=1.0, colour=None):
    """A fat ink splat: a lumpy filled shape with a dark outline."""
    colour = colour if colour is not None else idx("coral", 21)
    rnd = random.Random(seed)
    k = [(rnd.choice([3, 4, 5, 6]), rnd.uniform(0, 6.3), rnd.uniform(0.05, 0.14)) for _ in range(3)]
    pts = []
    for i in range(48):
        t = 2 * math.pi * i / 48
        rr = r * (1 + sum(a * math.sin(n * t + p) for n, p, a in k))
        pts.append((cx + rr * math.cos(t) * stretch, cy + rr * math.sin(t) * squash))
    cv.polygon(pts, colour)
    cv.outline(pts, idx("grey", 2), 2)
    return pts


def mouth(cv, cx, cy, w, open_):
    teeth = idx("grey", 30)
    dark = idx("grey", 1)
    if open_ <= 0:
        cv.line(cx - w, cy, cx + w, cy, dark, 2)
        return
    pts = [(cx - w, cy), (cx + w, cy), (cx + w * 0.7, cy + open_), (cx - w * 0.7, cy + open_)]
    cv.polygon(pts, dark)
    for x in range(int(cx - w + 2), int(cx + w - 1), 4):
        cv.polygon([(x, cy), (x + 3, cy), (x + 1.5, cy + 3)], teeth)


def blot_frames():
    W, H = 64, 56
    frames = {}
    # A-D: slithering -- lumps shifting, little drips trailing.
    for i, f in enumerate("ABCD"):
        cv = Canvas(W, H)
        sq = [1.0, 0.92, 1.0, 1.06][i]
        blob(cv, W / 2, 32, 20, seed=200 + i, stretch=1.1 / sq, squash=sq * 0.9)
        eyes(cv, W / 2, 26, 7, look=[1, 0, -1, 0][i])
        mouth(cv, W / 2, 36, 8, 0)
        for d in range(3):
            x = W / 2 - 14 + d * 14 + [0, 2, 0, -2][i]
            cv.line(x, 48, x, 53 - d, idx("coral", 18), 2)
        frames[f] = cv
    # E-G: the bite -- rearing up, jaws opening, snapping.
    for i, f in enumerate("EFG"):
        cv = Canvas(W, H)
        blob(cv, W / 2, 30 - 2 * i, 21 + i, seed=210 + i, squash=0.9 + 0.05 * i)
        eyes(cv, W / 2, 22 - 2 * i, 8)
        mouth(cv, W / 2, 34 - 2 * i, 11, [4, 10, 2][i])
        frames[f] = cv
    # H: hurt -- flinching, eyes crossed.
    cv = Canvas(W, H)
    blob(cv, W / 2, 34, 19, seed=220, stretch=1.2, squash=0.8)
    eyes(cv, W / 2, 30, 7, cross=True)
    frames["H"] = cv
    # I-N: melting into a puddle.
    for i, f in enumerate("IJKLMN"):
        cv = Canvas(W, H)
        t = (i + 1) / 6.0
        blob(cv, W / 2, 32 + 18 * t, 20 * (1 - 0.6 * t) + 2, seed=230 + i,
             stretch=1.1 + 1.0 * t, squash=max(0.12, 0.9 - 0.8 * t))
        if i < 2:
            eyes(cv, W / 2, 28 + 18 * t, 7, cross=True)
        frames[f] = cv
    return frames


# ------------------------------------------------------------------ pickups

def ink_bottle():
    cv = Canvas(14, 20)
    glass, ink, cork = idx("grey", 26), idx("ink", 18), idx("coral", 18)
    cv.polygon([(1, 8), (12, 8), (12, 19), (1, 19)], glass)
    cv.polygon([(2, 11), (11, 11), (11, 18), (2, 18)], ink)
    cv.polygon([(4, 3), (9, 3), (9, 8), (4, 8)], glass)
    cv.polygon([(4, 0), (9, 0), (9, 3), (4, 3)], cork)
    cv.outline([(1, 8), (12, 8), (12, 19), (1, 19)], idx("grey", 2))
    return cv


def spare_nib():
    cv = Canvas(12, 20)
    metal, edge = idx("grey", 28), idx("grey", 2)
    pts = [(6, 0), (11, 12), (9, 19), (3, 19), (1, 12)]
    cv.polygon(pts, metal)
    cv.outline(pts, edge)
    cv.line(6, 1, 6, 10, edge)
    cv.circle(6, 11, 1.5, edge)
    cv.polygon([(3, 16), (9, 16), (9, 19), (3, 19)], idx("lime", 22))
    return cv


def card_sheet(glint):
    cv = Canvas(22, 18)
    card, edge = idx("lime", 22), idx("grey", 2)
    pts = [(0, 6), (16, 0), (22, 10), (6, 17)]
    cv.polygon(pts, card)
    cv.outline(pts, edge)
    for k in range(4):
        cv.line(4 + k * 4, 6 + k, 9 + k * 4, 13 - k * 0, idx("coral", 20))
    if glint:
        cv.circle(15, 5, 2, idx("grey", 30), fill=idx("grey", 31))
    return cv


def sprite_lumps():
    lumps = []
    for f, cv in sorted(scribble_frames().items()):
        lumps.append(("POSS%s0" % f, world_patch(cv, hover=12)))
    for f, cv in sorted(blot_frames().items()):
        lumps.append(("SARG%s0" % f, world_patch(cv)))
    lumps.append(("CLIPA0", world_patch(ink_bottle())))
    lumps.append(("STIMA0", world_patch(spare_nib())))
    lumps.append(("ARM1A0", world_patch(card_sheet(False))))
    lumps.append(("ARM1B0", world_patch(card_sheet(True))))
    return lumps
