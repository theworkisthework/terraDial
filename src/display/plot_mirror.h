#pragma once
#include <lvgl.h>
#include "../net/fluidnc_client.h"

// The plot, drawn on the dial while the plotter draws it on paper.
//
// There's no G-code parsing here: FluidNC already reports where the head is
// ten times a second ($Report/Interval=100, see fluidnc_client.cpp), and
// whether the pen is on the paper is just which side of the pen-up/pen-down
// heights Z sits. So the mirror records the head's path while a job runs
// and draws the pen-down stretches of it. Between reports the head's path
// is drawn as a straight chord, which on a long curve at speed reads as a
// gently faceted curve -- a likeness of the plot, not a copy of it.
//
// The view fits itself to what's been drawn so far, so a small sketch
// fills the circle as well as a full-bed plot does, zooming out as the
// drawing grows.
//
// Main loop only (LVGL).
namespace PlotMirror
{
    // A round view of the given diameter. One per firmware: the Job
    // Progress screen owns it.
    lv_obj_t *create(lv_obj_t *parent, lv_coord_t diameter);

    // Call every loop iteration. Cheap when nothing has changed; records a
    // point whenever a new position report has arrived during a job, and
    // starts a fresh drawing when a new job does.
    void sample(const FluidNCStatus &st);

    // Puts what's been recorded since the last call on screen. Separate
    // from sample() so a burst of reports costs one redraw, not several.
    void refresh();

    // Anything drawn yet for the current (or last) job?
    bool hasDrawing();
}
