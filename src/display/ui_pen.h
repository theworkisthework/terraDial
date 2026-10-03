#pragma once
#include <lvgl.h>
#include <stdint.h>
lv_obj_t *uiPenCreate();

// Flips pen state (same effect as tapping either segment) -- wired to a
// knob click from ui_nav, per the mockup's "click knob toggles". Returns
// false if nothing was sent: the machine isn't idle, or the command for the
// new state is empty (Settings > Machine).
bool uiPenToggle();

// Sends the pen-up command whatever the tracked state says -- for the park
// sequence, which can't trust that state (it's only the panel's assumption,
// and can be wrong after a power cycle or another client moving the pen).
// Returns the command's FluidNCClient ticket, to follow FluidNC's reply to
// it, or 0 if nothing was sent (same reasons as uiPenToggle()).
uint32_t uiPenLift();

// Current pen state, for Job Progress's "pen down" status pill.
bool uiPenIsDown();
