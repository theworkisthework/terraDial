#pragma once
#include <stdint.h>

// The microphone, used for one thing: noticing that someone is talking.
// No recording, no recognition -- just loudness against the room's own
// background level, worked out a few dozen times a second and thrown
// away. It's only switched on while something asks it to listen (the About
// card does, for the easter egg's unlock) and goes quiet again by itself
// a moment after the asking stops.
//
// On boards without a mic (BOARD_HAS_MIC in pins.h) every call is a no-op
// and heardVoiceWithin() is always false.
namespace Mic
{
    void begin();

    // Keep listening for at least this long from now. Call it repeatedly
    // while listening matters; the mic switches off once it stops.
    void listenFor(uint32_t ms);

    // Did it hear something voice-loud in the last `ms` milliseconds?
    bool heardVoiceWithin(uint32_t ms);

    // Whether this board can hear at all.
    bool available();
}
