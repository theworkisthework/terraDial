#pragma once
#include <lvgl.h>

// The easter egg: a spirograph. Turn the knob to change the gears, watch
// the curve draw itself, then have the plotter draw it for real.
//
// Reached only by the unlock on Settings > About (see ui_settings.cpp) --
// it's not on the dial.
//
// The maths is the classic toy's: a gear of `gear` teeth rolling inside a
// ring of `ring` teeth, with the pen in a hole `pen`% of the way out from
// the gear's centre (a hypotrochoid). The curve closes after
// gear / gcd(ring, gear) trips round the ring, which is why nudging one
// setting by a single tooth can turn a simple flower into a dense lace.
lv_obj_t *uiSpiroCreate();
void uiSpiroOnShow();
void uiSpiroHandleRotate(int32_t delta);

// Every loop iteration, whatever's on screen: feeds a running plot to the
// machine, so leaving the screen doesn't abandon a drawing half-way.
void uiSpiroUpdate();
