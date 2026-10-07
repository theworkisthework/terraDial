#pragma once
#include <lvgl.h>

// The idle brand screen: the terraPen mark on an otherwise empty face, shown
// after a spell of no input and dismissed by the first touch or knob move.
//
// It sits between "in use" and "asleep" (see screen_sleep.h): the panel is
// on the machine and in view, so an idle dial may as well show the mark
// rather than whichever screen happened to be left open. Sleep still takes
// over later and turns the backlight off entirely.
//
// Drawn as an overlay on lv_layer_top() rather than as a screen in ui_nav's
// list, so it can appear over anything without disturbing where the user
// was -- dismissing it returns them exactly where they left off.
//
// The mark draws itself each time it appears (LogoDraw: a pen tracing the
// stroke), and the name and site fade in once the pen lifts.
namespace UiBrand
{
    void show();
    // The same screen as a boot splash: it takes itself away once the mark
    // has drawn and held for a moment. Any input dismisses it sooner, the
    // same way it dismisses the idle screen.
    void showSplash();
    void hide();
    bool isShown();
}
