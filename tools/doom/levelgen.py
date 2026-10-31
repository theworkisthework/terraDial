"""Procedural levels for terrapen.wad: rooms, corridors, doors, baddies.

A level is a grid of 12x12-cell slots with a room in each. Every room
covers its slot's centre, so two neighbouring rooms always share a centre
row (or column), and the corridor between them runs straight along it --
which puts each door in a clean one-cell gap. Rooms are joined as a random
spanning tree plus a few extra links, so there are loops to circle round.

You start in one room; the exit switch is in the room furthest away.
Later levels are bigger, with more baddies and more of them Blots.

Characters are the ones make_wad.py's legend understands:
  #  wall        .  room          ;  room, other walls     :  corridor
  ,  courtyard   p  porch         1 2 3  steps up to a dais
  D  door        X  exit switch (a wall)
  P  you         S  Scribble      B  Blot
  i  ink         h  spare nib     a  sheet of card
"""
import random

SLOT = 12


class Room:
    def __init__(self, i, j, left, top, w, h, kind):
        self.i, self.j = i, j
        self.left, self.top, self.w, self.h = left, top, w, h
        self.right, self.bottom = left + w - 1, top + h - 1
        self.cx, self.cy = i * SLOT + SLOT // 2, j * SLOT + SLOT // 2
        self.kind = kind
        self.links = []

    def cells(self):
        for r in range(self.top, self.bottom + 1):
            for c in range(self.left, self.right + 1):
                yield c, r


def generate(num, seed):
    """Rows of characters for level `num` (2 and up)."""
    rnd = random.Random(seed)
    cols = 2 + min(2, (num - 1) // 3)      # 2, 2, 3, 3, 3, 4 ...
    rows_n = 2 if num < 6 else 3
    W, H = cols * SLOT + 1, rows_n * SLOT + 1
    g = [["#"] * W for _ in range(H)]

    # Rooms: each covers its slot's centre and stays 2 cells inside the slot.
    rooms = {}
    start = (rnd.randrange(cols), rnd.randrange(rows_n))
    for i in range(cols):
        for j in range(rows_n):
            kind = "."
            if (i, j) != start:
                kind = rnd.choices([".", ";", ",", "hall"], [3, 3, 2, 2])[0]
            big = kind in (",", "hall")
            w = rnd.randint(7 if big else 5, 9)
            h = rnd.randint(5 if big else 4, 8)
            sx, sy = i * SLOT, j * SLOT
            cx, cy = sx + SLOT // 2, sy + SLOT // 2
            left = rnd.randint(max(sx + 2, cx - w + 2), min(cx - 1, sx + SLOT - 1 - w))
            top = rnd.randint(max(sy + 2, cy - h + 2), min(cy - 1, sy + SLOT - 1 - h))
            rooms[(i, j)] = Room(i, j, left, top, w, h, kind)

    # Links: a random spanning tree over neighbouring slots, plus a few more.
    def neighbours(k):
        i, j = k
        for n in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
            if n in rooms:
                yield n

    seen, stack, links = {start}, [start], set()
    while stack:
        k = stack[-1]
        nxt = [n for n in neighbours(k) if n not in seen]
        if not nxt:
            stack.pop()
            continue
        n = rnd.choice(nxt)
        links.add(tuple(sorted((k, n))))
        seen.add(n)
        stack.append(n)
    all_pairs = {tuple(sorted((k, n))) for k in rooms for n in neighbours(k)}
    spare = sorted(all_pairs - links)
    rnd.shuffle(spare)
    links |= set(spare[:1 + num // 4])

    # Carve rooms, then corridors and doors.
    for room in rooms.values():
        fill = "," if room.kind == "," else ";" if room.kind == ";" else "."
        for c, r in room.cells():
            g[r][c] = fill
    porch = []
    for a, b in sorted(links):
        ra, rb = rooms[a], rooms[b]
        if a[1] == b[1]:  # side by side: along the shared centre row
            left_room, right_room = (ra, rb) if ra.i < rb.i else (rb, ra)
            r = left_room.cy
            d1, d2 = left_room.right + 1, right_room.left - 1
            for c in range(d1 + 1, d2):
                g[r][c] = ":"
            g[r][d1] = g[r][d2] = "D"
            porch += [(left_room, left_room.right, r), (right_room, right_room.left, r)]
        else:             # one above the other: along the shared centre column
            top_room, bot_room = (ra, rb) if ra.j < rb.j else (rb, ra)
            c = top_room.cx
            d1, d2 = top_room.bottom + 1, bot_room.top - 1
            for r in range(d1 + 1, d2):
                g[r][c] = ":"
            g[d1][c] = g[d2][c] = "D"
            porch += [(top_room, c, top_room.bottom), (bot_room, c, bot_room.top)]
        ra.links.append(rb)
        rb.links.append(ra)
    # A courtyard's doors open onto a low porch, so they sit under a lintel.
    for room, c, r in porch:
        if room.kind == ",":
            g[r][c] = "p"

    # Halls get a stepped dais, entered from the west.
    for room in rooms.values():
        if room.kind != "hall":
            continue
        x0 = room.left + 2
        for dr in (-1, 0, 1):
            r = room.cy + dr
            g[r][x0], g[r][x0 + 1] = "1", "2"
            for c in range(x0 + 2, min(x0 + 4, room.right)):
                g[r][c] = "3"

    # The exit: in the room furthest from the start (never a courtyard: a
    # switch belongs on an indoor wall), set in its top wall.
    dist = {start: 0}
    queue = [start]
    while queue:
        k = queue.pop(0)
        for n in rooms[k].links:
            key = (n.i, n.j)
            if key not in dist:
                dist[key] = dist[k] + 1
                queue.append(key)
    candidates = [k for k in rooms if k != start and rooms[k].kind != ","]
    exit_room = rooms[max(candidates, key=lambda k: (dist[k], rnd.random()))]
    exit_cols = [c for c in range(exit_room.left, exit_room.right + 1) if c != exit_room.cx]
    g[exit_room.top - 1][rnd.choice(exit_cols)] = "X"

    # You, at the start room's centre.
    s = rooms[start]
    g[s.cy][s.cx] = "P"

    # Baddies and pickups, room by room.
    blot_share = min(0.6, 0.2 + 0.05 * num)
    for k, room in rooms.items():
        free = [(c, r) for c, r in room.cells() if g[r][c] in ".;," and (c, r) != (s.cx, s.cy)]
        rnd.shuffle(free)

        def place(ch):
            if free:
                c, r = free.pop()
                g[r][c] = ch

        if k != start:
            area = room.w * room.h
            for _ in range(1 + num // 3 + area // 30 + rnd.randint(0, 1)):
                place("B" if rnd.random() < blot_share else "S")
        if rnd.random() < 0.6:
            place("i")
        if rnd.random() < 0.4:
            place("h")
        if rnd.random() < 0.12:
            place("a")

    return ["".join(r) for r in g]


def reachable(rows, from_ch="P", to_ch="X"):
    """True if you can walk from from_ch to a cell beside to_ch."""
    solid = set("#X")
    H, W = len(rows), len(rows[0])
    start = next((c, r) for r in range(H) for c in range(W) if rows[r][c] == from_ch)
    seen, stack = {start}, [start]
    while stack:
        c, r = stack.pop()
        for nc, nr in ((c + 1, r), (c - 1, r), (c, r + 1), (c, r - 1)):
            if 0 <= nc < W and 0 <= nr < H and (nc, nr) not in seen:
                if rows[nr][nc] == to_ch:
                    return True
                if rows[nr][nc] not in solid:
                    seen.add((nc, nr))
                    stack.append((nc, nr))
    return False
