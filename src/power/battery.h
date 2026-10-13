#pragma once

#include <stdint.h>

// Battery level, on boards that run off one (BOARD_HAS_BATTERY in pins.h).
// Elsewhere status().present is simply false and nothing else is touched.
//
// Main loop only: update() samples the ADC every couple of seconds and
// status() is a plain cached read, cheap enough for every UI refresh.
struct BatteryStatus
{
    bool present = false;  // this board has a battery to report on
    bool external = false; // on USB power: the level can't be read then
    uint8_t percent = 0;   // 0-100, meaningful only when !external
    uint16_t millivolts = 0;
};

namespace Battery
{
    void begin();
    void update();
    const BatteryStatus &status();
}
