#pragma once

// Every screen is designed on a 240px grid -- the CrowPanel's panel, where
// the layout was first worked out -- and drawn at the panel's own
// resolution. px() converts one to the other. Every pixel quantity in the
// UI goes through it: sizes, positions, offsets, padding, radii, line and
// border widths, scroll steps, touch slop. Times, angles, colours, counts
// and lv_pct() values don't.
//
// So the numbers in the screens' code stay the ones that were tuned on the
// 240px panel, and the geometry comments around them (e.g. "the face's
// half-width at y=74 is sqrt(120^2 - 74^2) = 94px") still hold, in grid
// units. On the CrowPanel px(v) is exactly v, and the build is the same
// one it was before px() existed.
//
// Text gets the same treatment through UI_FONT_<n> / UI_ICONS_<n>: the
// font that's <n>px on the 240 grid, i.e. Montserrat <n> on the CrowPanel
// and the 1.5x size on the Knob. Never use lv_font_montserrat_* or
// lucide_* directly in a screen.

#include <lvgl.h>
#include "pins.h"

// Rounds to nearest (half away from zero). Exact identity when PANEL_RES ==
// UI_RES, for negative values too.
constexpr lv_coord_t px(int v)
{
    return (lv_coord_t)((v * PANEL_RES + (v >= 0 ? UI_RES / 2 : -(UI_RES / 2))) / UI_RES);
}

// For geometry that's worked in floats (ring radii, arc maths).
constexpr float pxf(float v) { return v * PANEL_RES / UI_RES; }

#if PANEL_RES == 240
#define UI_FONT_12 lv_font_montserrat_12
#define UI_FONT_14 lv_font_montserrat_14
#define UI_FONT_16 lv_font_montserrat_16
#define UI_FONT_18 lv_font_montserrat_18
#define UI_FONT_24 lv_font_montserrat_24
#define UI_FONT_32 lv_font_montserrat_32
#define UI_ICONS_12 lucide_12
#define UI_ICONS_14 lucide_14
#define UI_ICONS_16 lucide_16
#define UI_ICONS_18 lucide_18
#define UI_ICONS_24 lucide_24
#define UI_ICONS_32 lucide_32
#elif PANEL_RES == 360
// 14 * 1.5 = 21 and 18 * 1.5 = 27 aren't Montserrat sizes LVGL ships; the
// nearest even sizes stand in.
#define UI_FONT_12 lv_font_montserrat_18
#define UI_FONT_14 lv_font_montserrat_20
#define UI_FONT_16 lv_font_montserrat_24
#define UI_FONT_18 lv_font_montserrat_28
#define UI_FONT_24 lv_font_montserrat_36
#define UI_FONT_32 lv_font_montserrat_48
#define UI_ICONS_12 lucide_18
#define UI_ICONS_14 lucide_20
#define UI_ICONS_16 lucide_24
#define UI_ICONS_18 lucide_28
#define UI_ICONS_24 lucide_36
#define UI_ICONS_32 lucide_48
#else
#error "No font mapping for this PANEL_RES -- add one above (and the sizes to lv_conf.h)"
#endif
