#pragma once

#include <stdint.h>

// Runtime-editable settings, backed by NVS (Preferences, same pattern
// terraPixel already uses). Everything here is loaded once at boot
// (falling back to the compile-time defaults from secrets.h / jog_config.h
// the first time) and saved back to NVS whenever the Settings screen
// changes it. WiFi credentials now live here too (seeded from secrets.h on
// first boot) so the on-device Settings Wi-Fi card can edit them -- see
// net/wifi_manager.h for how a change here gets applied.
struct AppSettings
{
    char fluidNcHost[32];
    char terraPixelHost[32];
    // Idle seconds before the panel sleeps (backlight off). 0 = never.
    // Replaced an older "dim the backlight" timeout -- see
    // display/screen_sleep.h for why a real sleep mode earns its place.
    uint16_t sleepTimeoutSec;
    uint8_t backlightBrightnessPct; // 10-100, the "awake" backlight level
    // LED ring brightness while asleep: the ring keeps showing machine state
    // across the room once the screen is dark.
    uint8_t sleepLedBrightnessPct;
    // Show the terraPen mark after a shorter spell of inactivity, before the
    // panel sleeps proper (see display/ui_brand.h).
    bool showIdleLogo;
    // Flips which way the menu rings step relative to knob rotation.
    // Purely a menu-navigation preference -- jogging always follows the
    // physical direction of the knob, since that maps to real machine
    // movement and inverting it would be a safety hazard.
    bool invertMenuRotation;
    // Sent as-is by the Pen screen (see PEN_UP_CMD in jog_config.h). Swapped
    // wholesale for a machine whose pen lifts the other way, rather than
    // flipping a sign somewhere -- the commands stay what the user typed.
    // The spirograph's entry at the top of the Jobs list. Off moves it to
    // Settings > About instead, for anyone who'd rather Jobs held only jobs.
    bool spiroInJobs;
    char penUpCmd[48];
    char penDownCmd[48];
    char wifiSsid[33];
    char wifiPass[64];
};

namespace Config
{
    void begin();
    AppSettings &get();
    void save();
}
