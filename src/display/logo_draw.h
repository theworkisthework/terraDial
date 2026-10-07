#pragma once
#include <lvgl.h>

// The terraPen logo, drawn the way a plotter would draw it: one continuous
// stroke traced from end to end at a steady pen speed, with the pen itself
// -- a small accent-coloured dot -- leading the line, then lifting off when
// the mark is finished. It's the identity's own animation
// (TP-Logo-Animated.svg: a dot tracing the path), rebuilt from the same
// stroke (logo_stroke.h).
//
// Same footprint as iconLogo: a 128px box on the 240 grid, so it drops in
// wherever that bitmap sat.
namespace LogoDraw
{
    // Starts drawing straight away. The returned object is the logo's box;
    // position it like any other object. Deleting it (or its parent) stops
    // the animation and frees everything.
    lv_obj_t *create(lv_obj_t *parent, lv_color_t stroke, uint32_t drawMs = 3200);
}
