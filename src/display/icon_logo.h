#pragma once
#include <lvgl.h>

// The terraPen logo (theworkisthework/terrapen-identity, TP-Logo-Animated.svg)
// rasterised to an LV_IMG_CF_ALPHA_8BIT bitmap: 128x128 on the 240px panel, 192x192 on the 360px panel, matching
// the panel this build is for (PANEL_RES in pins.h).
//
// Alpha-only, so it has no colour of its own -- LVGL paints it with the
// object's `img_recolor`. That's how it reads correctly on this panel: the
// source artwork is a black stroke for print, and here it's drawn in a
// palette colour on the dark background without any image editing.
//
// Drawn 1:1. Do NOT scale it with lv_img_set_zoom -- transforming an image
// whose parent also has opacity < 255 sends LVGL down an offscreen-layer
// path that proved unreliable on this board.
//
// Regenerate with: python tools/gen_logo.py
extern const lv_img_dsc_t iconLogo;

// One byte per iconLogo pixel: how far along the logo's single stroke the
// pen is when it first inks that pixel, 0..254 (the earliest pass, where the
// stroke crosses itself), or 255 for a pixel it never touches. LogoDraw
// reveals iconLogo by this to draw the mark in, pen-style, with the
// finished frame identical to the static bitmap.
static const uint8_t ICON_LOGO_ARRIVAL_MAX = 254;
static const uint8_t ICON_LOGO_NEVER = 255;
extern const uint8_t iconLogoArrival[];
