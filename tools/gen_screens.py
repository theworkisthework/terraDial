#!/usr/bin/env python3
"""Generate SVG illustrations of each terraDial screen for the README.

These are RENDERS, not screenshots: there's no way to capture the real
panel's framebuffer from a build machine. Everything here is transcribed
from the layout code -- palette hex from include/palette.h, ring radii and
chip sizes from radial_ring.h/ui_dial.cpp, arc angles from the setArcLayout()
calls, hub sizes and font sizes from each ui_*.cpp -- so the proportions and
colours match what the firmware draws. Icons are the real Lucide glyphs the
firmware's lucide_* fonts carry, at the font size each label uses.

Keep in sync by hand if a screen's geometry changes.

Usage: python tools/gen_screens.py   ->  docs/screens/*.svg
"""
import math
import os

# --- palette (include/palette.h, ported from terraForge's dark theme) ---
BG_APP = "#1a1a2e"
BG_PANEL = "#16213e"
BG_SECONDARY = "#0f3460"
BORDER = "#5c7a9e"
TEXT = "#e0e0e0"
TEXT_MUTED = "#9ca3af"
TEXT_FAINT = "#818ea5"
ACCENT = "#e94560"
ACCENT_FG = "#ffffff"
ACCENT_SECONDARY = "#60a0ff"
ALERT = "#d12b3f"
GREEN = "#3ddc84"

W = 240
FONT = "Segoe UI, Roboto, Helvetica, Arial, sans-serif"

# --- ring geometry, from radial_ring.h defaults + per-screen overrides ---
DIAL_RADIUS, DIAL_NEAR, DIAL_FAR = 84, 62, 34  # ui_dial.cpp RING_*
DIAL_HUB = 96                                  # ui_dial.cpp HUB_SIZE
DIAL_SPREAD = 0.3   # RadialRing::setSpread on the home dial
ARC_SPREAD = 0.55   # ...and on the Jobs/Settings arcs


def head(parts):
    parts.append(
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240" width="240" height="240">'
    )
    # bezel + face: the panel is round, so anything past r=120 is unreachable
    parts.append('<circle cx="120" cy="120" r="119" fill="#0a0a12"/>')
    parts.append('<circle cx="120" cy="120" r="116" fill="%s"/>' % BG_APP)
    parts.append('<clipPath id="face"><circle cx="120" cy="120" r="116"/></clipPath>')
    parts.append('<g clip-path="url(#face)">')


def tail(parts):
    parts.append("</g></svg>")


def text(parts, x, y, s, size=12, fill=TEXT, weight="400", anchor="middle"):
    parts.append(
        '<text x="%g" y="%g" font-family="%s" font-size="%g" font-weight="%s" '
        'fill="%s" text-anchor="%s" dominant-baseline="central">%s</text>'
        % (x, y, FONT, size, weight, fill, anchor, esc(s))
    )


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def circle(parts, cx, cy, r, fill, stroke=None, sw=1, opacity=None):
    op = ' opacity="%g"' % opacity if opacity is not None else ""
    st = ' stroke="%s" stroke-width="%g"' % (stroke, sw) if stroke else ""
    parts.append('<circle cx="%g" cy="%g" r="%g" fill="%s"%s%s/>' % (cx, cy, r, fill, st, op))


def rect(parts, x, y, w, h, r, fill, opacity=None, stroke=None):
    op = ' opacity="%g"' % opacity if opacity is not None else ""
    st = ' stroke="%s" stroke-width="1"' % stroke if stroke else ""
    parts.append(
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="%s"%s%s/>'
        % (x, y, w, h, r, fill, st, op)
    )


def mix(c1, c2, t):
    """Blend hex colours -- mirrors lv_color_mix(c1, c2, t*255)."""
    a = [int(c1[i : i + 2], 16) for i in (1, 3, 5)]
    b = [int(c2[i : i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(b[i] + (a[i] - b[i]) * t) for i in range(3))


# ---------------------------------------------------------------- icons
# Lucide icons (lucide.dev, ISC; notices in src/display/fonts/LICENSE-lucide.txt),
# copied from lucide-static at the version tools/gen_lucide_font.py pins, so
# these are the same glyphs the firmware's lucide_* fonts draw. 24x24 boxes,
# stroked at 2 -- the font scales both with its pixel size, and so does icon().
LUCIDE = {
    "house": '<path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/>'
             '<path d="M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    "move": '<path d="M12 2v20"/><path d="m15 19-3 3-3-3"/><path d="m19 9 3 3-3 3"/>'
            '<path d="M2 12h20"/><path d="m5 9-3 3 3 3"/><path d="m9 5 3-3 3 3"/>',
    "pen": '<path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"/>',
    "card-sim": '<path d="M12 14v4"/>'
                '<path d="M14.172 2a2 2 0 0 1 1.414.586l3.828 3.828A2 2 0 0 1 20 7.828V20a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z"/>'
                '<path d="M8 14h8"/><rect x="8" y="10" width="8" height="8" rx="2"/>',
    "folder": '<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"/>',
    "file": '<path d="M6 22a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h8a2.4 2.4 0 0 1 1.704.706l3.588 3.588A2.4 2.4 0 0 1 20 8v12a2 2 0 0 1-2 2z"/>'
            '<path d="M14 2v5a1 1 0 0 0 1 1h5"/>',
    "camera": '<path d="M13.997 4a2 2 0 0 1 1.76 1.05l.486.9A2 2 0 0 0 18.003 7H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2h1.997a2 2 0 0 0 1.759-1.048l.489-.904A2 2 0 0 1 10.004 4z"/>'
              '<circle cx="12" cy="13" r="3"/>',
    "lightbulb": '<path d="M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5"/>'
                 '<path d="M9 18h6"/><path d="M10 22h4"/>',
    "settings": '<path d="M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915"/>'
                '<circle cx="12" cy="12" r="3"/>',
    "octagon-x": '<path d="m15 9-6 6"/>'
                 '<path d="M2.586 16.726A2 2 0 0 1 2 15.312V8.688a2 2 0 0 1 .586-1.414l4.688-4.688A2 2 0 0 1 8.688 2h6.624a2 2 0 0 1 1.414.586l4.688 4.688A2 2 0 0 1 22 8.688v6.624a2 2 0 0 1-.586 1.414l-4.688 4.688a2 2 0 0 1-1.414.586H8.688a2 2 0 0 1-1.414-.586z"/>'
                 '<path d="m9 9 6 6"/>',
    "square": '<rect width="18" height="18" x="3" y="3" rx="2"/>',
    "pause": '<rect x="14" y="3" width="5" height="18" rx="1"/><rect x="5" y="3" width="5" height="18" rx="1"/>',
    "play": '<path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"/>',
    "wifi": '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/>'
            '<path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
    "sliders-horizontal": '<path d="M10 5H3"/><path d="M12 19H3"/><path d="M14 3v4"/><path d="M16 17v4"/>'
                          '<path d="M21 12h-9"/><path d="M21 19h-5"/><path d="M21 5h-7"/><path d="M8 10v4"/><path d="M8 12H3"/>',
    "monitor": '<rect width="20" height="14" x="2" y="3" rx="2"/>'
               '<line x1="8" x2="16" y1="21" y2="21"/><line x1="12" x2="12" y1="17" y2="21"/>',
    "info": '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>',
    "eye": '<path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0"/>'
           '<circle cx="12" cy="12" r="3"/>',
    "delete": '<path d="M10 5a2 2 0 0 0-1.344.519l-6.328 5.74a1 1 0 0 0 0 1.481l6.328 5.741A2 2 0 0 0 10 19h10a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2z"/>'
              '<path d="m12 9 6 6"/><path d="m18 9-6 6"/>',
    "check": '<path d="M20 6 9 17l-5-5"/>',
    "x": '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    "chevron-left": '<path d="m15 18-6-6 6-6"/>',
    "chevron-right": '<path d="m9 18 6-6-6-6"/>',
    "rotate-ccw": '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>',
    "arrow-up-down": '<path d="m21 16-4 4-4-4"/><path d="M17 20V4"/><path d="m3 8 4-4 4 4"/><path d="M7 4v16"/>',
    "triangle-alert": '<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3"/>'
                      '<path d="M12 9v4"/><path d="M12 17h.01"/>',
}


def icon(parts, cx, cy, name, px, col):
    """A Lucide glyph at a lucide_<px> font's size, centred on (cx, cy)."""
    parts.append(
        '<g transform="translate(%g %g) scale(%g)" fill="none" stroke="%s" stroke-width="2" '
        'stroke-linecap="round" stroke-linejoin="round">%s</g>'
        % (cx - px / 2.0, cy - px / 2.0, px / 24.0, col, LUCIDE[name])
    )


def ring_icon_px(nearness):
    """uiRingIconFont(uiRingIconSize(...)) in ui_widgets.cpp, settled
    (no hysteresis): lucide_32 in the top slot, 24 mid-ring, 14 beyond."""
    if nearness > 0.90:
        return 32
    return 24 if nearness > 0.52 else 14


def back_button(parts):
    """The shared bottom-centre back chip (ui_screen_shell.cpp)."""
    circle(parts, 120, 214, 18, BG_SECONDARY)
    icon(parts, 120, 214, "chevron-left", 16, TEXT_MUTED)


def estop_pip(parts):
    """The stop pip beside it, on screens that can move the machine
    (ui_screen_shell.cpp's addEstopButton)."""
    circle(parts, 82, 208, 13, ALERT)
    icon(parts, 82, 208, "octagon-x", 12, ACCENT_FG)


# ------------------------------------------------------- ring renderers
def spread_angle(ang, amount, limit=180.0):
    """RadialRing::spreadAngle -- opens the spacing up near the top slot and
    squeezes it shut near the bottom, pinning both ends in place."""
    if not amount:
        return ang
    u = ang / limit
    return limit * (u + amount * math.sin(math.pi * u) / math.pi)


def full_ring(parts, items, selected, radius=DIAL_RADIUS, near=DIAL_NEAR, far=DIAL_FAR,
              opa_far=100 / 255.0, spread=DIAL_SPREAD):
    """Home's full circle (RadialRing default mode), with the dial's spread."""
    n = len(items)
    step = 360.0 / n
    # Far items first, so the bunched-up bottom stacks the way the firmware
    # orders it (nearer chips in front).
    order = sorted(range(n), key=lambda i: -abs(((i - selected) * step + 180) % 360 - 180))
    for i in order:
        kind, alert = items[i]
        ang = spread_angle(((i - selected) * step + 180) % 360 - 180, spread)
        nearness = 1 - abs(ang) / 180.0
        size = far + (near - far) * nearness
        rad = math.radians(ang)
        cx, cy = 120 + radius * math.sin(rad), 120 - radius * math.cos(rad)
        if alert:
            circle(parts, cx, cy, size / 2, ALERT)
            icon(parts, cx, cy, kind, ring_icon_px(nearness), ACCENT_FG)
        else:
            circle(parts, cx, cy, size / 2, mix(ACCENT, BG_SECONDARY, nearness),
                   opacity=opa_far + (1 - opa_far) * nearness)
            icon(parts, cx, cy, kind, ring_icon_px(nearness), mix(ACCENT_FG, TEXT_MUTED, nearness))


def arc_ring(parts, kinds, selected, step_deg, half_arc, radius=80, near=60, far=20,
             opa_far=0.0, spread=ARC_SPREAD):
    """Jobs/Settings open arc -- bottom left clear for the back button."""
    # Far chips first, so the bunched-up tail stacks the way the firmware
    # orders it (nearer chips in front).
    for i in sorted(range(len(kinds)), key=lambda i: -abs((i - selected) * step_deg)):
        kind = kinds[i]
        ang = (i - selected) * step_deg
        if abs(ang) >= half_arc:
            continue
        ang = spread_angle(ang, spread, half_arc)
        nearness = max(0.0, 1 - abs(ang) / half_arc)
        size = far + (near - far) * nearness
        rad = math.radians(ang)
        cx, cy = 120 + radius * math.sin(rad), 120 - radius * math.cos(rad)
        circle(parts, cx, cy, size / 2, mix(ACCENT, BG_SECONDARY, nearness),
               opacity=opa_far + (1 - opa_far) * nearness)
        icon(parts, cx, cy, kind, ring_icon_px(nearness), mix(ACCENT_FG, TEXT_MUTED, nearness))


def hub(parts, size, lines):
    """Centre hub: (text, dy, size, colour, weight) tuples."""
    circle(parts, 120, 120, size / 2, BG_SECONDARY, BORDER, 1)
    for s, dy, sz, col, wt in lines:
        text(parts, 120, 120 + dy, s, sz, col, wt)


# ------------------------------------------------------------- screens
def screen_home():
    p = []
    head(p)
    # Must match DIAL_ITEMS in src/display/ui_dial.cpp:
    # Home XY, Jog, Pen, Jobs, Photo, E-Stop, Lights, Settings.
    items = [("house", 0), ("move", 0), ("pen", 0), ("card-sim", 0),
             ("camera", 0), ("octagon-x", 1), ("lightbulb", 0), ("settings", 0)]
    full_ring(p, items, 0)
    hub(p, DIAL_HUB, [("Home XY", -8, 14, TEXT, "600"), ("IDLE", 12, 12, TEXT_MUTED, "400")])
    tail(p)
    return "home-dial", p


def screen_jobs():
    p = []
    head(p)
    # A folder or two ahead of the files, as an SD root usually lists them.
    arc_ring(p, ["folder", "folder", "file", "file", "file", "file", "file"], 2, 30.0, 132.0)
    hub(p, 96, [("flow_red.gcode", -18, 11, TEXT, "600"),
                ("8.2 MB", 4, 12, TEXT_MUTED, "400")])
    icon(p, 104, 144, "play", 12, ACCENT)
    text(p, 126, 144, "Run", 12, ACCENT, "600")
    back_button(p)
    tail(p)
    return "jobs", p


def screen_jog():
    p = []
    head(p)
    for i, (lbl, sel) in enumerate((("X", 0), ("Y", 1), ("Z", 0))):
        x = 103 + i * 40
        rect(p, x - 17, 32, 34, 26, 13, ACCENT if sel else "none")
        text(p, x, 45, lbl, 14, ACCENT_FG if sel else BORDER, "600")
    text(p, 120, 90, "12.40", 32, TEXT, "600")
    rect(p, 74, 118, 92, 24, 12, BG_SECONDARY)
    text(p, 120, 130, "Set Y zero", 12, TEXT_MUTED)
    # Four chips at 36 wide + 6 gaps = 162px, centred on 120 (see ui_jog.cpp).
    for i, (lbl, sel) in enumerate((("0.1", 0), ("1", 1), ("10", 0), ("100", 0))):
        x = 39 + i * 42
        rect(p, x, 160, 36, 28, 14, ACCENT if sel else BG_SECONDARY)
        text(p, x + 18, 174, lbl, 12, ACCENT_FG if sel else TEXT_MUTED, "600")
    back_button(p)
    estop_pip(p)
    tail(p)
    return "jog", p


def screen_pen():
    p = []
    head(p)
    icon(p, 106, 34, "pen", 14, TEXT_MUTED)
    text(p, 126, 34, "PEN", 12, TEXT_MUTED)
    rect(p, 50, 88, 140, 64, 20, BG_PANEL)
    rect(p, 53, 91, 66, 58, 17, ACCENT)
    text(p, 86, 111, "Pen", 16, ACCENT_FG, "600")
    text(p, 86, 130, "up", 16, ACCENT_FG, "600")
    rect(p, 121, 91, 66, 58, 17, BG_SECONDARY)
    text(p, 154, 111, "Pen", 16, TEXT_MUTED, "600")
    text(p, 154, 130, "down", 16, TEXT_MUTED, "600")
    back_button(p)
    tail(p)
    return "pen", p


def screen_home_confirm():
    p = []
    head(p)
    icon(p, 120, 62, "house", 24, ACCENT)
    text(p, 120, 92, "Clear the bed first", 16, TEXT, "600")
    text(p, 120, 112, "Lift the pen and check the", 12, TEXT_MUTED)
    text(p, 120, 126, "carriage can move freely.", 12, TEXT_MUTED)
    rect(p, 40, 138, 160, 38, 19, ACCENT)
    text(p, 120, 157, "Confirm & home", 14, ACCENT_FG, "600")
    back_button(p)
    tail(p)
    return "home-confirm", p


def screen_lights():
    p = []
    head(p)
    text(p, 120, 52, "LIGHTS", 12, ACCENT_SECONDARY, "600")
    circle(p, 96, 72, 5, TEXT_FAINT)
    text(p, 132, 72, "Rail: IDLE", 12, TEXT_MUTED)
    rows = [("Panel ring brightness", .6), ("Rail brightness", .8)]
    y = 92
    for lbl, frac in rows:
        text(p, 120, y, lbl, 12, TEXT_MUTED)
        rect(p, 30, y + 10, 180, 10, 5, BG_PANEL)
        rect(p, 30, y + 10, 180 * frac, 10, 5, ACCENT)
        circle(p, 30 + 180 * frac, y + 15, 8, ACCENT_FG)
        y += 38
    text(p, 120, y, "Film mode", 12, TEXT_MUTED)
    rect(p, 98, y + 10, 44, 24, 12, BG_PANEL)
    circle(p, 110, y + 22, 9, ACCENT_FG)
    rect(p, 30, y + 44, 180, 30, 15, ACCENT)
    text(p, 120, y + 59, "Party: OFF", 12, ACCENT_FG, "600")
    back_button(p)
    tail(p)
    return "lights", p


def screen_settings_ring():
    p = []
    head(p)
    arc_ring(p, ["wifi", "sliders-horizontal", "monitor", "info"], 0, 40.0, 132.0,
             radius=76, near=64, far=26, opa_far=110 / 255.0)
    hub(p, 82, [("Wi-Fi", -8, 14, TEXT, "600"), ("open", 12, 12, ACCENT, "400")])
    back_button(p)
    tail(p)
    return "settings-ring", p


def slider(parts, y, pct):
    """uiMakeSlider (ui_widgets.cpp): 10px bg-panel track, accent fill,
    18px white knob. The track fills the 176px row; the knob's travel is
    inset one knob radius from each end, so its edge stops at the track's
    ends, and the fill runs from the track's left end to the knob."""
    knob_r = 9
    rect(parts, 32, y, 176, 10, 5, BG_PANEL)
    cx = 32 + knob_r + (176 - 2 * knob_r) * pct / 100.0
    rect(parts, 32, y, cx - 32, 10, 5, ACCENT)
    circle(parts, cx, y + 5, knob_r, ACCENT_FG)


def switch(parts, y, on):
    """uiMakeSwitch (ui_widgets.cpp): 44x24, accent when on."""
    rect(parts, 32, y, 44, 24, 12, ACCENT if on else BG_PANEL)
    circle(parts, 64 if on else 44, y + 12, 9, ACCENT_FG)


# makeDisplayCard, drawn as two screenfuls like Machine. Rows are
# uiMakeRow flex columns, so labels and controls sit at the left edge.
def screen_settings_display():
    p = []
    head(p)
    text(p, 120, 50, "DISPLAY", 12, ACCENT_SECONDARY, "600")
    text(p, 32, 72, "Brightness: 100%", 12, TEXT_MUTED, anchor="start")
    slider(p, 82, 100)
    text(p, 32, 108, "Idle logo", 12, TEXT_MUTED, anchor="start")
    switch(p, 118, True)
    text(p, 32, 158, "Invert menu rotation", 12, TEXT_MUTED, anchor="start")
    switch(p, 168, False)
    back_button(p)
    tail(p)
    return "settings-display", p


def screen_settings_display_sleep():
    p = []
    head(p)
    text(p, 32, 58, "Invert menu rotation", 12, TEXT_MUTED, anchor="start")
    switch(p, 68, False)
    text(p, 32, 110, "Sleep after", 12, TEXT_MUTED, anchor="start")
    for i, (lbl, sel) in enumerate((("Never", 0), ("3m", 0), ("5m", 1), ("10m", 0))):
        x = 32 + i * 42
        rect(p, x, 120, 38, 26, 13, ACCENT if sel else BG_PANEL)
        text(p, x + 19, 133, lbl, 11, ACCENT_FG if sel else TEXT_MUTED, "600")
    text(p, 32, 162, "Ring sleep brightness: 50%", 12, TEXT_MUTED, anchor="start")
    slider(p, 172, 50)
    back_button(p)
    tail(p)
    return "settings-display-sleep", p


def text_field(parts, y, label, value):
    """uiMakeTextField (ui_widgets.cpp): muted caption over a bg-panel box,
    1px border, 8px corners -- terraForge's form input. Left-aligned, as the
    row's flex column lays it out."""
    text(parts, 32, y, label, 12, TEXT_MUTED, anchor="start")
    rect(parts, 32, y + 9, 176, 26, 8, BG_PANEL, stroke=BORDER)
    text(parts, 43, y + 22, value, 12, TEXT, anchor="start")


def secondary_button(parts, y, icon_name, label):
    """makeSecondaryButton (ui_settings.cpp): full-width bg-secondary pill,
    lucide_12 icon leading the text. Centred on an estimated text width --
    Montserrat 12 averages ~6.3px a character."""
    rect(parts, 30, y, 180, 30, 15, BG_SECONDARY)
    w = 12 + 5 + len(label) * 6.3
    x = 120 - w / 2
    icon(parts, x + 6, y + 15, icon_name, 12, TEXT)
    text(parts, x + 17, y + 15, label, 12, TEXT, anchor="start")


def screen_settings_wifi():
    # makeWifiCard: SSID (tap -> network picker), password, status, Connect,
    # Forget network. Centred, as the panel's flex column lays them out.
    p = []
    head(p)
    text(p, 120, 50, "WI-FI", 12, ACCENT_SECONDARY, "600")
    text(p, 114, 72, "SSID: Studio", 12, TEXT)
    icon(p, 157, 72, "chevron-right", 12, TEXT)
    text(p, 120, 92, "Password: (tap to edit)", 12, TEXT)
    text(p, 120, 112, "Connected", 12, TEXT_MUTED)
    rect(p, 65, 124, 110, 34, 17, ACCENT)
    text(p, 120, 141, "Connect", 16, ACCENT_FG)
    rect(p, 45, 164, 150, 28, 14, BG_SECONDARY)
    text(p, 120, 178, "Forget network", 12, TEXT)
    back_button(p)
    tail(p)
    return "settings-wifi", p


def screen_settings_wifi_scan():
    # openScanOverlay: full-face overlay on the top layer, so the screen's
    # back button is covered -- the X (or a knob long-press) closes it. The
    # knob highlight starts on the first network.
    p = []
    head(p)
    circle(p, 120, 28, 14, BG_SECONDARY)
    icon(p, 120, 28, "x", 12, TEXT)
    text(p, 120, 54, "Select a network", 14, TEXT, "600")
    for i, (name, sel) in enumerate((("Studio", 1), ("Workshop-5G", 0), ("terrapen-guest", 0))):
        y = 74 + i * 34
        rect(p, 25, y, 190, 34, 0, ACCENT if sel else BG_PANEL)
        col = ACCENT_FG if sel else TEXT
        icon(p, 45, y + 17, "wifi", 16, col)
        text(p, 61, y + 17, name, 14, col, anchor="start")
    rect(p, 70, 194, 100, 30, 15, BG_SECONDARY)
    text(p, 120, 209, "Rescan", 12, TEXT)
    tail(p)
    return "settings-wifi-scan", p


# The Machine card is taller than the face, so it's drawn three times: at
# the top, scrolled to the pen section, and scrolled to the end.
def screen_settings_machine_host():
    p = []
    head(p)
    text(p, 120, 50, "MACHINE", 12, ACCENT_SECONDARY, "600")
    text_field(p, 70, "Host / IP", "terrapen")
    text(p, 120, 132, "PEN", 12, ACCENT_SECONDARY, "600")
    text_field(p, 150, "Pen up command", "G90 G21 G0 Z5")
    back_button(p)
    tail(p)
    return "settings-machine-host", p


def screen_settings_machine_pen():
    p = []
    head(p)
    text(p, 120, 50, "PEN", 12, ACCENT_SECONDARY, "600")
    text_field(p, 70, "Pen up command", "G90 G21 G0 Z5")
    text_field(p, 118, "Pen down command", "G90 G21 G0 Z0")
    secondary_button(p, 162, "arrow-up-down", "Swap up / down")
    back_button(p)
    tail(p)
    return "settings-machine-pen", p


def screen_settings_machine_terrapixel():
    p = []
    head(p)
    secondary_button(p, 46, "arrow-up-down", "Swap up / down")
    secondary_button(p, 84, "rotate-ccw", "Reset to defaults")
    text(p, 120, 132, "TERRAPIXEL", 12, ACCENT_SECONDARY, "600")
    text_field(p, 152, "Host / IP", "terrapen-leds")
    back_button(p)
    tail(p)
    return "settings-machine-terrapixel", p


def screen_job_progress():
    p = []
    head(p)
    circle(p, 120, 120, 106, "none", BG_PANEL, 12)
    pct = 0.42
    a0, a1 = -90, -90 + 360 * pct
    large = 1 if pct > .5 else 0
    x0, y0 = 120 + 106 * math.cos(math.radians(a0)), 120 + 106 * math.sin(math.radians(a0))
    x1, y1 = 120 + 106 * math.cos(math.radians(a1)), 120 + 106 * math.sin(math.radians(a1))
    p.append('<path d="M%g %g A106 106 0 %d 1 %g %g" stroke="%s" stroke-width="12" fill="none"/>'
             % (x0, y0, large, x1, y1, ACCENT))
    text(p, 120, 64, "flow_red.gcode", 12, TEXT_MUTED)
    text(p, 120, 84, "6:18   ~9 min left", 12, TEXT_MUTED)
    text(p, 120, 106, "42%", 32, TEXT, "600")
    circle(p, 86, 164, 29, BG_SECONDARY)
    icon(p, 86, 164, "pause", 24, TEXT)
    circle(p, 154, 164, 29, ALERT)
    icon(p, 154, 164, "square", 24, ACCENT_FG)
    tail(p)
    return "job-progress", p


def screen_estop():
    p = []
    head(p)
    circle(p, 120, 106, 78, ALERT)
    icon(p, 120, 76, "octagon-x", 24, ACCENT_FG)
    text(p, 120, 104, "E-STOP", 18, ACCENT_FG, "700")
    text(p, 120, 128, "Feed hold", 12, ACCENT_FG)
    text(p, 120, 142, "+ soft reset", 12, ACCENT_FG)
    back_button(p)
    tail(p)
    return "estop", p


def screen_alarm():
    p = []
    head(p)
    icon(p, 120, 62, "triangle-alert", 24, ACCENT)
    text(p, 120, 92, "Alarm active", 16, TEXT, "600")
    text(p, 120, 112, "Clear the bed, then clear", 12, TEXT_MUTED)
    text(p, 120, 126, "the alarm to continue.", 12, TEXT_MUTED)
    rect(p, 40, 138, 160, 38, 19, ACCENT)
    text(p, 120, 157, "Clear alarm", 14, ACCENT_FG, "600")
    back_button(p)
    tail(p)
    return "alarm-clear", p


# The terraPen mark: theworkisthework/terrapen-identity Logo/TP-Logo-Animated.svg,
# the same single stroked path tools/gen_logo.py rasterises for the firmware
# (450 viewBox, stroke 3), with its whitespace collapsed.
LOGO_D = (
    "M168,137.6l28.5,16.4v0v65.8l28.5,16.4l0,0c0,8.8,0,24.1,0,32.9l0,0l-28.5-16.4"
    "V302c0,10.2,5.4,19.6,14.3,24.7 l14.2,8.2v32.9v0l-28.5-16.4c-7.8-4.5-16.2-12."
    "2-21.3-21.1c-5.2-9.1-7.2-19.4-7.2-28.2v-65.7l-28.5-16.4v-32.9l28.5,16.4L168,"
    "137.6 L168,137.6l92.6-53.4c4.7-2.7,10.1-2.4,14.2,0l28.5,16.4l0,0c4.1,2.4,7.1"
    ",6.8,7.1,12.3v90.5c0,5.1-1.3,9.9-3.8,14.2 c-2.4,4.2-6,7.8-10.4,10.4L225,269."
    "1l0,16.4c0,10.2,5.4,19.7,14.3,24.8l14.2,8.2v32.9L225,367.8v0l0,0l-28.5-16.4 "
    "c-7.8-4.5-16.2-12.2-21.3-21.1c-5.2-9.1-7.2-19.4-7.2-28.2v-65.7l-28.5-16.4v-3"
    "2.9l28.5-16.4v16.4l-14.2-8.2l14.2-8.2L168,137.6 l14.2-8.2l28.5,16.4l0,0l0,65"
    ".8l28.5,16.4V261l-14.2,8.2l-14.2-8.2l0,32.8c0,10.2,5.4,19.6,14.3,24.7l14.2,8"
    ".2v32.8l14.2-8.2v-32.9 l-14.2-8.2c-8.8-5.1-14.3-14.5-14.3-24.8l0-16.4l28.5-1"
    "6.4v-32.9L225,203.4v-32.9v-32.9l-28.5-16.4l14.2-8.2l28.5,16.4v0v32.9 l-14.2,"
    "8.2v32.9l14.3-8.3l28.5,16.4l-0.1,0.1v32.8l14.2-8.2v-32.8l-32.3-18.7c-2.4,4.2"
    "-6,7.8-10.3,10.3l-14.4,8.3v-32.9l28.5-16.4 l0-32.9L225,104.7l14.2-8.2l28.5,1"
    "6.4l0,0v35v0c-2.4,0-4.8,0.6-7.1,1.9l-7.2,4.2v16.5v0c0,5-1.3,9.9-3.8,14.1l32."
    "3,18.7l0-41.4 c-0.1-10.8-11.9-17.5-21.3-12.1l-7.2,4.2v16.5l28.5,16.4v-16.6l-"
    "28.5-16.4l7.2-4.2c9.4-5.4,21.1,1.3,21.3,12.1v-57.3l-28.5-16.4"
    "l7.1-4.1c4.7-2.7,10.1-2.4,14.2,0l21.4,12.3l0.1,131.5"
)
LOGO_VIEWBOX = 450.0


def logo_mark(parts, cx, cy, box, col):
    """The logo image the firmware shows (icon_logo, box x box px, centred on
    cx, cy). The stroke gets gen_logo.py's MIN_STROKE_PX floor, since the
    print artwork's 3 units scale to under a pixel at panel size."""
    k = box / LOGO_VIEWBOX
    stroke_px = max(1.5, 3.0 * k)
    parts.append(
        '<path transform="translate(%g %g) scale(%g)" d="%s" fill="none" stroke="%s" '
        'stroke-width="%g" stroke-linecap="round" stroke-linejoin="round"/>'
        % (cx - box / 2.0, cy - box / 2.0, k, LOGO_D, col, stroke_px / k)
    )


def qr_block(parts, x, y, size, modules=21):
    """Illustrative QR -- deterministic pattern, not a real encoding."""
    rect(parts, x - 4, y - 4, size + 8, size + 8, 2, "#ffffff")
    m = size / float(modules)
    seed = 12345
    for r in range(modules):
        for c in range(modules):
            finder = ((r < 7 and c < 7) or (r < 7 and c >= modules - 7)
                      or (r >= modules - 7 and c < 7))
            if finder:
                edge = (r in (0, 6) or c in (0, 6) or r in (modules - 1, modules - 7)
                        or c in (modules - 1, modules - 7))
                ring = (2 <= r % (modules - 7) <= 4) if False else None
                on = edge or (2 <= (r % 7) <= 4 and 2 <= (c % 7) <= 4)
            else:
                seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                on = (seed >> 16) & 1
            if on:
                parts.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s"/>'
                             % (x + c * m, y + r * m, m, m, BG_APP))


def screen_about():
    p = []
    head(p)
    text(p, 120, 46, "ABOUT", 12, ACCENT_SECONDARY, "600")
    logo_mark(p, 120, 84, 64, TEXT)
    text(p, 120, 120, "terraPen", 16, TEXT, "600")
    text(p, 120, 138, "terrapen.xyz", 12, ACCENT)
    qr_block(p, 88, 152, 64)
    tail(p)
    return "about", p


def screen_brand():
    p = []
    head(p)
    logo_mark(p, 120, 102, 128, TEXT)
    text(p, 120, 182, "terraPen", 16, TEXT, "600")
    text(p, 120, 204, "terrapen.xyz", 12, ACCENT)
    tail(p)
    return "idle-brand", p


def screen_keyboard():
    """radial_keyboard.cpp: the ring turns so the selected key sits at the
    top. Keys get a share of the circle by width (keyWeight), the spread
    opens the top, and keys shrink and fade in bands with distance
    (fontForKey) -- all mirrored here."""
    p = []
    head(p)
    radius, spread, opa_far = 100, 0.35, 110 / 255.0
    # Page one of a password field: letters, then the action keys.
    keys = [(c, "char") for c in "abcdefghijklmnopqrstuvwxyz"] + [
        ("ABC", "word"), ("SP", "word"), ("delete", "icon"),
        ("eye", "icon"), ("check", "icon"), ("x", "icon")]
    weight = {"char": 1.0, "icon": 1.3, "word": 1.6}
    sel = 7

    total = sum(weight[k] for _, k in keys)
    centres, acc = [], 0.0
    for _, kind in keys:
        centres.append(360.0 * (acc - weight[keys[0][1]] / 2 + weight[kind] / 2) / total)
        acc += weight[kind]

    def font_px(kind, is_sel, nearness):
        if kind == "word":
            return 24 if is_sel else (14 if nearness > 0.6 else 12)
        if kind == "icon":
            return 24 if is_sel else (16 if nearness > 0.6 else 14 if nearness > 0.3 else 12)
        return 32 if is_sel else (18 if nearness > 0.6 else 14 if nearness > 0.3 else 12)

    for i, (label, kind) in enumerate(keys):
        ang = (centres[i] - centres[sel] + 180) % 360 - 180
        ang = spread_angle(ang, spread)
        nearness = 1 - abs(ang) / 180.0
        rad = math.radians(ang)
        cx, cy = 120 + radius * math.sin(rad), 120 - radius * math.cos(rad)
        is_sel = i == sel
        px = font_px(kind, is_sel, nearness)
        col = ACCENT if is_sel else TEXT_MUTED
        opa = 1.0 if is_sel else opa_far + (1 - opa_far) * nearness
        p.append('<g opacity="%.3f">' % opa)
        if kind == "icon":
            icon(p, cx, cy, label, px, col)
        else:
            text(p, cx, cy, label, px, col, "600" if is_sel else "400")
        p.append("</g>")

    hub(p, 156, [("Password", -42, 14, TEXT_MUTED, "400"),
                 ("******g", -10, 18, TEXT, "600"),
                 ("h", 30, 24, ACCENT, "700")])
    tail(p)
    return "radial-keyboard", p


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "docs", "screens")
    os.makedirs(out, exist_ok=True)
    for fn in (screen_home, screen_jobs, screen_jog, screen_pen, screen_home_confirm,
               screen_lights, screen_settings_ring, screen_settings_wifi, screen_settings_wifi_scan,
               screen_settings_machine_host, screen_settings_machine_pen,
               screen_settings_machine_terrapixel, screen_settings_display,
               screen_settings_display_sleep,
               screen_job_progress, screen_estop, screen_alarm, screen_keyboard,
               screen_about, screen_brand):
        name, parts = fn()
        path = os.path.join(out, name + ".svg")
        open(path, "w").write("\n".join(parts))
        print("wrote docs/screens/%s.svg" % name)


if __name__ == "__main__":
    main()
