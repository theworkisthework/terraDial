#pragma once

// Vibration feedback, on boards that have it (BOARD_HAS_HAPTICS in pins.h:
// a DRV2605 on the Waveshare Knob). Everywhere else every call is a no-op,
// so callers don't need their own #ifs.
//
// Main loop only. The DRV2605 sits on the touch controller's I2C bus
// (Wire1), and the touch driver is read from lv_timer_handler() on the
// same loop -- keeping both on one task means the bus never needs a lock.
namespace Haptics
{
    // Call once, after touch.begin() has brought Wire1 up. Finding no
    // DRV2605 just leaves haptics off; it's a nicety, never a boot failure.
    void begin();

    // One short click using the preset effect loaded at begin().
    void detent();

    // A brief real-time pulse at 75% of the DRV2605's RTP amplitude.
    void pulse75();

    // Call from loop() to end real-time pulses without blocking UI work.
    void update();
}
