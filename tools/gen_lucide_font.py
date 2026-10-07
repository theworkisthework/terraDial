#!/usr/bin/env python3
"""Build LVGL fonts of Lucide icons (lucide.dev) for the firmware.

Pulls Lucide's icon font from the pinned `lucide-static` npm package and
runs LVGL's own converter (lv_font_conv) over it, once per text size the
UI uses. Each output font holds only the icons listed in ICONS below and
falls back to the same-size Montserrat for everything else, so
`lucide_24` is a drop-in replacement for `lv_font_montserrat_24`: a label
set to it draws Lucide icons *and* ordinary text, which is what lets a
string like LUCIDE_PLAY " Run" work in one label.

Lucide's glyphs sit in the private-use range 0xE000-0xE6xx; LVGL's built-in
FontAwesome symbols (LV_SYMBOL_*) are at 0xF000 and up. No overlap, so the
two can coexist during the move from one to the other.

The generated .c files are committed, so a normal firmware build needs
neither Node nor network access -- only regenerating does.

Each font also takes its Montserrat twin's line metrics, and its icons are
shifted to sit centred in that line (see match_montserrat()), so swapping
fonts doesn't move anything and icons line up with the text beside them.

Licensing: Lucide is ISC-licensed, and some of its icons derive from Feather
(MIT). Both require their notices to travel with every copy, so the package's
LICENSE is copied verbatim to src/display/fonts/LICENSE-lucide.txt and each
generated font opens with a notice pointing to it.

Usage:  python3 tools/gen_lucide_font.py
Needs:  node + npx (fetches lucide-static and lv_font_conv on first run),
        and a prior `pio run` so LVGL's Montserrat sources are in .pio/libdeps
Writes: src/display/fonts/lucide_<size>.c, src/display/fonts/LICENSE-lucide.txt,
        src/display/lucide_icons.h

To add an icon: find its name on lucide.dev, add it to ICONS, rerun.
"""
import glob
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

LUCIDE_VERSION = "1.48.0"
LV_FONT_CONV_VERSION = "1.5.3"

# Every Montserrat size lv_conf.h can enable -- one Lucide font per size,
# each falling back to its Montserrat twin. 12-32 are the 240px panel's
# sizes; 20, 28, 36 and 48 are the ones the 360px panel adds when its
# layout scales them by 1.5 (see include/ui_scale.h). Each font is only
# compiled when lv_conf.h enables its twin, so a board pays for the sizes
# it uses and no others.
SIZES = [12, 14, 16, 18, 20, 24, 28, 32, 36, 48]

# Lucide names, as on lucide.dev. Each becomes a LUCIDE_<NAME> macro.
ICONS = [
    # Home dial destinations
    "house",            # Home XY
    "move",             # Jog
    "pen",              # Pen
    "pen-line",
    "card-sim",         # Jobs (Lucide has no SD card; this is the nearest shape)
    "folder",
    "folder-open",
    "file",
    "camera",           # Photo
    "lightbulb",        # Lights
    "settings",         # Settings
    # Stop / job control
    "circle-stop",
    "octagon-x",
    "square",
    "pause",
    "play",
    # Settings categories
    "wifi",
    "wifi-cog",
    "monitor",          # Display
    "info",             # About
    # Candidates for the machine-settings category
    "sliders-horizontal",
    "wrench",
    "cpu",
    "cog",
    "pencil-ruler",
    "drafting-compass",
    "printer",
    "router",
    # General UI
    "eye",
    "eye-off",
    "x",
    "check",
    "delete",           # backspace
    "chevron-left",
    "chevron-right",
    "arrow-left",
    "refresh-cw",
    "triangle-alert",
    "trash-2",
    "arrow-up-down",    # swap pen up/down commands (Settings > Machine)
    "rotate-ccw",       # reset pen commands to defaults (Settings > Machine)
]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(HERE, "src", "display", "fonts")
HEADER = os.path.join(HERE, "src", "display", "lucide_icons.h")


def run(cmd, cwd=None):
    # npm and npx are .cmd scripts on Windows, which subprocess won't find
    # by bare name; which() resolves them through PATHEXT.
    cmd = [shutil.which(cmd[0]) or cmd[0]] + cmd[1:]
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        sys.exit("failed: " + " ".join(cmd))
    return result.stdout


def fetch_lucide(workdir):
    """lucide-static@LUCIDE_VERSION unpacked into workdir; returns its root."""
    tgz = run(["npm", "pack", "lucide-static@" + LUCIDE_VERSION, "--silent"], cwd=workdir).strip().splitlines()[-1]
    with tarfile.open(os.path.join(workdir, tgz)) as tar:
        tar.extractall(workdir)
    return os.path.join(workdir, "package")


FONT_NOTICE = """/*
 * Icon glyphs from Lucide %s (https://lucide.dev), converted for LVGL by
 * tools/gen_lucide_font.py. GENERATED -- do not edit.
 *
 * Copyright (c) 2026 Lucide Icons and Contributors -- ISC License.
 * Icons derived from Feather: Copyright (c) 2013-present Cole Bemis -- MIT License.
 * Full licence texts: LICENSE-lucide.txt, alongside this file.
 */

"""


def montserrat_metrics(size):
    """(line_height, base_line) of LVGL's built-in lv_font_montserrat_<size>."""
    found = glob.glob(os.path.join(HERE, ".pio", "libdeps", "*", "lvgl", "src", "font",
                                   "lv_font_montserrat_%d.c" % size))
    if not found:
        sys.exit("LVGL sources not found -- run `pio run` once first")
    src = open(found[0], encoding="utf-8").read()
    return (int(re.search(r"\.line_height = (\d+)", src).group(1)),
            int(re.search(r"\.base_line = (-?\d+)", src).group(1)))


def match_montserrat(c_src, size):
    """Give a generated Lucide font its Montserrat twin's line metrics.

    lv_font_conv sizes the line to Lucide's em box with the baseline at the
    bottom. But a label's line comes from its primary font, and the text in
    these labels is Montserrat drawn through the fallback -- so left as-is,
    swapping a label to lucide_N would shrink its line and drop text onto a
    baseline with no room for descenders. Copying the metrics keeps every
    label exactly the height it was.

    Icons are then moved down so the centre of their em box lands on the
    centre of the line box: the same place LVGL centres its own symbols, so
    an icon sits level with the text next to it.
    """
    line_height, base_line = montserrat_metrics(size)
    line_centre = line_height / 2.0 - base_line  # above the baseline
    shift = math.floor(line_centre - size / 2.0)
    c_src = re.sub(r"\.line_height = \d+,", ".line_height = %d," % line_height, c_src)
    c_src = re.sub(r"\.base_line = -?\d+,", ".base_line = %d," % base_line, c_src)
    c_src = re.sub(r"\.ofs_y = (-?\d+)\}", lambda m: ".ofs_y = %d}" % (int(m.group(1)) + shift), c_src)
    return c_src


def guard(c_src, size):
    """Compile the font only when its Montserrat fallback is enabled.

    lv_font_conv already wraps the font in `#if LUCIDE_<size>`, defaulting
    that to 1; default it to LV_FONT_MONTSERRAT_<size> instead. Without its
    twin a font won't build (the fallback is undeclared), and a board that
    doesn't use the size shouldn't carry it anyway.
    """
    default = "#define LUCIDE_%d 1\n" % size
    if default not in c_src:
        sys.exit("unexpected lv_font_conv output: no %r to retarget" % default.strip())
    return c_src.replace(default, "#define LUCIDE_%d LV_FONT_MONTSERRAT_%d\n" % (size, size), 1)


def macro_name(icon):
    return "LUCIDE_" + icon.upper().replace("-", "_")


def utf8_escape(codepoint):
    return "".join("\\x%02X" % b for b in chr(codepoint).encode("utf-8"))


def main():
    with tempfile.TemporaryDirectory() as work:
        pkg = fetch_lucide(work)
        codepoints = json.load(open(os.path.join(pkg, "font", "codepoints.json"), encoding="utf-8"))
        shutil.copy(os.path.join(pkg, "LICENSE"), os.path.join(FONT_DIR, "LICENSE-lucide.txt"))
        missing = [name for name in ICONS if name not in codepoints]
        if missing:
            sys.exit("not in lucide-static %s: %s" % (LUCIDE_VERSION, ", ".join(missing)))

        # Converted inside the temp dir with bare filenames: lv_font_conv
        # records its command line in the output, and absolute paths there
        # would change the committed files on every run.
        shutil.copy(os.path.join(pkg, "font", "lucide.ttf"), os.path.join(work, "lucide.ttf"))
        ranges = ",".join("0x%X" % codepoints[name] for name in ICONS)
        os.makedirs(FONT_DIR, exist_ok=True)
        for size in SIZES:
            name = "lucide_%d" % size
            run(["npx", "--yes", "lv_font_conv@" + LV_FONT_CONV_VERSION,
                 "--font", "lucide.ttf", "-r", ranges,
                 "--size", str(size), "--bpp", "4", "--format", "lvgl",
                 # lv_conf.h leaves LV_USE_FONT_COMPRESSED off
                 "--no-compress",
                 "--lv-include", "lvgl.h",
                 "--lv-font-name", name,
                 "--lv-fallback", "lv_font_montserrat_%d" % size,
                 "-o", name + ".c"], cwd=work)
            c_src = guard(match_montserrat(open(os.path.join(work, name + ".c"), encoding="utf-8").read(), size), size)
            open(os.path.join(FONT_DIR, name + ".c"), "w", encoding="utf-8", newline="\n").write(FONT_NOTICE % LUCIDE_VERSION + c_src)
            print("wrote src/display/fonts/%s.c" % name)

    width = max(len(macro_name(n)) for n in ICONS)
    lines = [
        "#pragma once",
        "#include <lvgl.h>",
        "",
        "// Lucide icons (lucide.dev) as LVGL font glyphs. Lucide is ISC-licensed, with",
        "// some icons derived from Feather (MIT); notices in fonts/LICENSE-lucide.txt.",
        "// GENERATED by tools/gen_lucide_font.py from lucide-static %s -- edit" % LUCIDE_VERSION,
        "// the ICONS list there and rerun rather than editing this file.",
        "//",
        "// lucide_<size> draws these icons and falls back to lv_font_montserrat_<size>",
        "// for everything else, so it replaces the Montserrat font of the same size",
        "// on any label that shows an icon -- including mixed strings such as",
        "// LUCIDE_PLAY \" Run\". The icons only render in a lucide_* font.",
        "",
    ]
    lines += ["LV_FONT_DECLARE(lucide_%d)" % size for size in SIZES]
    lines.append("")
    for name in ICONS:
        lines.append('#define %-*s "%s" // %s' % (width, macro_name(name), utf8_escape(codepoints[name]), name))
    open(HEADER, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("wrote src/display/lucide_icons.h (%d icons x %d sizes)" % (len(ICONS), len(SIZES)))


if __name__ == "__main__":
    main()
