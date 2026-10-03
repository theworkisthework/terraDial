#pragma once
#include <lvgl.h>
lv_obj_t *uiPenCreate();

// Flips pen state (same effect as tapping either segment) -- wired to a
// knob click from ui_nav, per the mockup's "click knob toggles". Returns
// false if nothing was sent: the machine isn't idle, or the command for the
// new state is empty (Settings > Machine).
bool uiPenToggle();

// Current pen state, for Job Progress's "pen down" status pill.
bool uiPenIsDown();
