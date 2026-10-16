#pragma once
#include <lvgl.h>

// Asteroids. The ship sits in the middle of the dial
// and turns with the knob; rocks drift across the glass; a touch anywhere
// fires. A long touch (or a long knob press) asks whether to quit, and the
// game holds still while it waits for the answer.
//
// Opened from the Asteroids button on Settings > About -- it's not on the
// dial.
lv_obj_t *uiAsteroidsCreate();

// Starts a fresh game each visit.
void uiAsteroidsOnShow();

// Knob turn: rotates the ship. Raw delta, like Jog -- clockwise turns the
// ship clockwise whatever the menu direction preference says.
void uiAsteroidsHandleRotate(int32_t delta);

// Knob click: fires, restarts after a game over, or keeps playing from the
// quit prompt.
void uiAsteroidsHandleClick();

// Knob long press: opens the quit prompt, or confirms it if already open.
void uiAsteroidsHandleLongPress();
