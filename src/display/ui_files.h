#pragma once
#include <lvgl.h>

lv_obj_t *uiFilesCreate();

// Becoming focused triggers a fresh SD listing.
void uiFilesSetFocused(bool focused);

// Called by ui_nav only while this screen is focused.
void uiFilesHandleRotate(int32_t delta);     // step the file ring
void uiFilesHandleSelect();                  // run the centered card's file directly
void uiFilesHandleDoubleClick();             // open the Run/Delete/Cancel confirm instead
                                              // (long-press stays the universal "back", so
                                              // it's not reused here)
// Back (knob long-press). Steps up one folder and returns true, or returns
// false at the SD root -- ui_nav should only leave the screen then.
bool uiFilesHandleBack();

// How many entries come before the files at the SD root: 1 while the
// spirograph sits at the top of Jobs, else 0. For the demo tour, whose
// scripted turns count files.
int uiFilesSpiroSlots();

// Call every loop iteration: checks for a completed SD listing and
// rebuilds the ring chips when one arrives.
void uiFilesUpdate();
