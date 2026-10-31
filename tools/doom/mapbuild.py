"""Turn a level drawn as a grid of characters into Doom map lumps.

Each character is a 64-unit square. Connected runs of the same floor
character become one sector; the edges between different sectors become
linedefs (one-sided against solid cells, two-sided between sectors); and a
BSP tree is built over the result, which is what lets the engine draw a
level of many rooms in the right order.

Every edge lies on the grid, so partition lines are grid lines and every
split lands on a grid point: the whole thing stays in exact integers.

The legend (what each character means) is passed in by make_wad.py.
"""
import math
import struct

CELL = 64


def name8(s):
    return s.encode("ascii").ljust(8, b"\0")[:8]


# ------------------------------------------------------------------ grid -> sectors and lines

class Level:
    def __init__(self, rows, terrain, things, wall_for):
        """rows: the map, top row first. terrain: char -> sector properties
        (dict with floor, ceil, floorpic, ceilpic, light, door?) or None for
        solid. things: char -> (thing type, terrain char). wall_for: sector
        properties -> wall texture name."""
        width = max(len(r) for r in rows)
        self.rows = [r.ljust(width) for r in rows]
        self.w, self.h = width, len(rows)
        self.terrain, self.things, self.wall_for = terrain, things, wall_for

    def ground(self, c, r):
        """The terrain character under cell (c, r); None when solid."""
        if not (0 <= c < self.w and 0 <= r < self.h):
            return None
        ch = self.rows[r][c]
        if ch in self.things:
            # A thing stands on whatever floor surrounds it.
            ch = self.things[ch][1]
            for nx, ny in ((c + 1, r), (c - 1, r), (c, r + 1), (c, r - 1)):
                if 0 <= nx < self.w and 0 <= ny < self.h:
                    n = self.rows[ny][nx]
                    if n not in self.things and self.terrain.get(n) is not None and not self.terrain[n].get("door"):
                        ch = n
                        break
        return ch if self.terrain.get(ch) is not None else None

    def build_sectors(self):
        """Flood-fill same-terrain regions into sectors. Door cells are
        each their own sector even when adjacent, one door per opening."""
        self.sector_of = {}
        self.sectors = []
        for r in range(self.h):
            for c in range(self.w):
                ch = self.ground(c, r)
                if ch is None or (c, r) in self.sector_of:
                    continue
                props = self.terrain[ch]
                sid = len(self.sectors)
                self.sectors.append(props)
                stack = [(c, r)]
                self.sector_of[(c, r)] = sid
                while stack:
                    x, y = stack.pop()
                    if props.get("door"):
                        continue
                    for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                        if (nx, ny) not in self.sector_of and self.ground(nx, ny) == ch:
                            self.sector_of[(nx, ny)] = sid
                            stack.append((nx, ny))

    def sector_at(self, c, r):
        return self.sector_of.get((c, r))

    @staticmethod
    def pt(c, r):
        """Grid corner (c, r) in map units: x right, y up."""
        return (c * CELL, -r * CELL)

    def build_lines(self):
        """Unit edges between differing cells, then merged into runs."""
        edges = []  # (v1, v2, front sector, back sector or None)

        # Vertical edges: between (c-1, r) on the left and (c, r) on the right.
        for c in range(self.w + 1):
            for r in range(self.h):
                left, right = self.sector_at(c - 1, r), self.sector_at(c, r)
                if left == right:
                    continue
                top, bottom = self.pt(c, r), self.pt(c, r + 1)
                # A line's front is on its right. Going up (bottom -> top),
                # the right is +x.
                if left is None:
                    edges.append((bottom, top, right, None))
                elif right is None:
                    edges.append((top, bottom, left, None))
                else:
                    edges.append((bottom, top, right, left))

        # Horizontal edges: between (c, r-1) above and (c, r) below.
        for r in range(self.h + 1):
            for c in range(self.w):
                above, below = self.sector_at(c, r - 1), self.sector_at(c, r)
                if above == below:
                    continue
                west, east = self.pt(c, r), self.pt(c + 1, r)
                # Going east, the right is -y (below).
                if above is None:
                    edges.append((west, east, below, None))
                elif below is None:
                    edges.append((east, west, above, None))
                else:
                    edges.append((west, east, below, above))

        # Doors want their lines' fronts facing out of the door sector.
        fixed = []
        for v1, v2, f, b in edges:
            if b is not None and self.sectors[f].get("door") and not self.sectors[b].get("door"):
                v1, v2, f, b = v2, v1, b, f
            fixed.append((v1, v2, f, b))

        # Merge collinear neighbours with the same sectors on the same sides.
        by_start = {}
        for e in fixed:
            by_start.setdefault((e[0], e[2], e[3]), []).append(e)
        used = set()
        lines = []
        for e in fixed:
            if id(e) in used:
                continue
            used.add(id(e))
            v1, v2, f, b = e
            d = (v2[0] - v1[0], v2[1] - v1[1])
            # Extend forwards while the next unit edge carries on in line.
            while True:
                nxt = [n for n in by_start.get((v2, f, b), [])
                       if id(n) not in used and (n[1][0] - n[0][0], n[1][1] - n[0][1]) == d]
                if not nxt:
                    break
                used.add(id(nxt[0]))
                v2 = nxt[0][1]
            lines.append([v1, v2, f, b])
        # Extend backwards for runs that started mid-way.
        merged = True
        while merged:
            merged = False
            ends = {(l[1], l[2], l[3]): l for l in lines}
            for l in lines:
                prev = ends.get((l[0], l[2], l[3]))
                if prev is not None and prev is not l:
                    d1 = (prev[1][0] - prev[0][0], prev[1][1] - prev[0][1])
                    d2 = (l[1][0] - l[0][0], l[1][1] - l[0][1])
                    if (d1[0] == 0) == (d2[0] == 0) and (d1[0] > 0) == (d2[0] > 0) and (d1[1] > 0) == (d2[1] > 0):
                        prev[1] = l[1]
                        lines.remove(l)
                        merged = True
                        break
        self.lines = lines


# ------------------------------------------------------------------ BSP

class Seg:
    __slots__ = ("v1", "v2", "line", "side", "offset")

    def __init__(self, v1, v2, line, side, offset):
        self.v1, self.v2, self.line, self.side, self.offset = v1, v2, line, side, offset


def side_of(px, py, seg):
    """>0 right (front), <0 left (back), 0 on the line."""
    (x1, y1), (x2, y2) = seg.v1, seg.v2
    return (x2 - x1) * (y1 - py) - (y2 - y1) * (x1 - px)


def classify(seg, part):
    a = side_of(seg.v1[0], seg.v1[1], part)
    b = side_of(seg.v2[0], seg.v2[1], part)
    if a >= 0 and b >= 0:
        if a == 0 and b == 0:
            same = ((seg.v2[0] - seg.v1[0]) * (part.v2[0] - part.v1[0]) +
                    (seg.v2[1] - seg.v1[1]) * (part.v2[1] - part.v1[1])) > 0
            return "front" if same else "back"
        return "front"
    if a <= 0 and b <= 0:
        return "back"
    return "split"


def split(seg, part):
    """Cut seg where it crosses part's line; both lines are axis-aligned."""
    (x1, y1), (x2, y2) = seg.v1, seg.v2
    if part.v1[0] == part.v2[0]:  # vertical partition
        x = part.v1[0]
        p = (x, y1)
    else:
        y = part.v1[1]
        p = (x1, y)
    a = Seg(seg.v1, p, seg.line, seg.side, seg.offset)
    b = Seg(p, seg.v2, seg.line, seg.side, seg.offset + abs(p[0] - x1) + abs(p[1] - y1))
    return a, b


def bbox(segs):
    xs = [v[0] for s in segs for v in (s.v1, s.v2)]
    ys = [v[1] for s in segs for v in (s.v1, s.v2)]
    return (max(ys), min(ys), min(xs), max(xs))  # top, bottom, left, right


class Bsp:
    def __init__(self, segs):
        self.segs = []
        self.subsectors = []
        self.nodes = []
        self.root = self.build(segs)

    def build(self, segs):
        best, best_cost = None, None
        for cand in segs:
            fronts = backs = splits = 0
            for s in segs:
                k = classify(s, cand)
                if k == "front":
                    fronts += 1
                elif k == "back":
                    backs += 1
                else:
                    splits += 1
            if backs + splits == 0 or fronts + splits == 0:
                continue  # doesn't divide anything
            cost = splits * 8 + abs(fronts - backs)
            if best_cost is None or cost < best_cost:
                best, best_cost = cand, cost
        if best is None:  # convex: a subsector
            first = len(self.segs)
            self.segs.extend(segs)
            self.subsectors.append((len(segs), first))
            return 0x8000 | (len(self.subsectors) - 1)

        front, back = [], []
        for s in segs:
            k = classify(s, best)
            if k == "front":
                front.append(s)
            elif k == "back":
                back.append(s)
            else:
                a, b = split(s, best)
                (front if side_of(a.v1[0], a.v1[1], best) > 0 or side_of(a.v2[0], a.v2[1], best) > 0 else back).append(a)
                (front if side_of(b.v1[0], b.v1[1], best) > 0 or side_of(b.v2[0], b.v2[1], best) > 0 else back).append(b)
        fbox, bbox_ = bbox(front), bbox(back)
        right = self.build(front)
        left = self.build(back)
        self.nodes.append((best.v1[0], best.v1[1], best.v2[0] - best.v1[0], best.v2[1] - best.v1[1],
                           fbox, bbox_, right, left))
        return len(self.nodes) - 1


# ------------------------------------------------------------------ lumps

def build_map(name, level, door_texture="DOOR1"):
    level.build_sectors()
    level.build_lines()

    verts = []
    vindex = {}

    def vid(p):
        if p not in vindex:
            vindex[p] = len(verts)
            verts.append(p)
        return vindex[p]

    linedefs, sidedefs, segs = [], [], []
    for i, (v1, v2, f, b) in enumerate(level.lines):
        fp = level.sectors[f]
        if b is None:
            sidedefs.append((0, 0, "-", "-", level.wall_for(fp), f))
            linedefs.append((vid(v1), vid(v2), 1, 0, 0, len(sidedefs) - 1, -1))
        else:
            bp = level.sectors[b]
            special = 1 if bp.get("door") else 0  # DR: open, wait, close
            flags = 4 | (8 if special else 0)  # two-sided; doors unpegged on top
            upper_f = door_texture if bp.get("door") else level.wall_for(fp)
            sidedefs.append((0, 0, upper_f, level.wall_for(fp), "-", f))
            sidedefs.append((0, 0, level.wall_for(bp), level.wall_for(bp), "-", b))
            linedefs.append((vid(v1), vid(v2), flags, special, 0, len(sidedefs) - 2, len(sidedefs) - 1))
        segs.append(Seg(v1, v2, i, 0, 0))
        if b is not None:
            segs.append(Seg(v2, v1, i, 1, 0))

    bsp = Bsp(segs)

    things = bytearray()
    for r, row in enumerate(level.rows):
        for c, ch in enumerate(row):
            if ch in level.things:
                ttype, _, angle = level.things[ch] if len(level.things[ch]) == 3 else (*level.things[ch], 90)
                x, y = c * CELL + CELL // 2, -r * CELL - CELL // 2
                things += struct.pack("<hhhhh", x, y, angle, ttype, 7)

    def bam(v1, v2):
        a = int(round(math.atan2(v2[1] - v1[1], v2[0] - v1[0]) / (2 * math.pi) * 65536)) & 0xFFFF
        return a - 65536 if a > 32767 else a

    seg_bytes = bytearray()
    for s in bsp.segs:
        seg_bytes += struct.pack("<hhhhhh", vid(s.v1), vid(s.v2), bam(s.v1, s.v2), s.line, s.side, s.offset)
    ss_bytes = b"".join(struct.pack("<hh", n, first) for n, first in bsp.subsectors)
    node_bytes = bytearray()
    for x, y, dx, dy, fb, bb, right, left in bsp.nodes:
        node_bytes += struct.pack("<hhhh", x, y, dx, dy)
        node_bytes += struct.pack("<hhhh", *fb) + struct.pack("<hhhh", *bb)
        node_bytes += struct.pack("<HH", right, left)

    sector_bytes = b"".join(
        struct.pack("<hh8s8shhh", p["floor"], p.get("ceil_closed", p["ceil"]) if p.get("door") else p["ceil"],
                    name8(p["floorpic"]), name8(p["ceilpic"]), p["light"], 0, 0)
        for p in level.sectors)
    line_bytes = b"".join(struct.pack("<hhhhhhh", *l) for l in linedefs)
    side_bytes = b"".join(struct.pack("<hh8s8s8sh", xo, yo, name8(u), name8(lo), name8(m), s)
                          for xo, yo, u, lo, m, s in sidedefs)
    vert_bytes = b"".join(struct.pack("<hh", x, y) for x, y in verts)
    n = len(level.sectors)
    reject = bytes((n * n + 7) // 8)
    blockmap = make_blockmap(verts, [(l[0], l[1]) for l in linedefs])
    return [(name, b""), ("THINGS", bytes(things)), ("LINEDEFS", line_bytes), ("SIDEDEFS", side_bytes),
            ("VERTEXES", vert_bytes), ("SEGS", bytes(seg_bytes)), ("SSECTORS", ss_bytes),
            ("NODES", bytes(node_bytes)), ("SECTORS", sector_bytes), ("REJECT", reject), ("BLOCKMAP", blockmap)]


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
    offs, body = [], []
    pos = header
    for hit in lists:
        offs.append(pos)
        entry = [0] + hit + [-1]
        body.append(struct.pack("<%dh" % len(entry), *entry))
        pos += len(entry)
    return struct.pack("<hhhh", ox, oy, cols, rows) + struct.pack("<%dH" % len(offs), *offs) + b"".join(body)
