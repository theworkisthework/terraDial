#pragma once

// Board selection. Each PlatformIO env defines exactly one BOARD_* (see
// platformio.ini), and that board's header supplies its pin map plus the
// facts the rest of the firmware branches on:
//
//   BOARD_NAME                 human-readable, for the About card and logs
//   PANEL_RES                  the panel's native width = height, in px.
//                              LVGL runs at this; the screens are designed
//                              on a 240px grid (UI_RES) and px() in
//                              ui_scale.h converts.
//   BOARD_PANEL_EVEN_WINDOWS   the panel wants every redraw window to start
//                              and end on even pixels
//   BOARD_HAS_KNOB_BUTTON      the knob also clicks. Without it, every
//                              knob click/long-press has a touch
//                              equivalent already (each screen's own
//                              buttons and the shell's back button).
//   BOARD_HAS_LED_RING         WS2812 status ring (src/led/panel_ring)
//   BOARD_HAS_SCREEN_RING      no LEDs, so the status ring is drawn round
//                              the edge of the display instead
//   BOARD_HAS_POWER_RAILS      GPIOs that must go high before the panel
//                              lights at all
//   BOARD_HAS_HAPTICS          DRV2605 vibration driver on the touch bus
//   BOARD_HAS_BATTERY          runs off a LiPo, with its voltage on an ADC
//                              pin (src/power/battery)
//   BOARD_ENCODER_QUADRATURE   1: standard A/B quadrature, 4 ticks per
//                              detent. 0: "bidirectional switch" knob --
//                              each detent pulses A (one way) or B (the
//                              other), never both.
//   BOARD_OTA_ASSET_NAME       the release asset this board's firmware
//                              updates itself from. Must differ per board,
//                              or an OTA could flash one board's image
//                              onto the other.

#if defined(BOARD_CROWPANEL_1_28)
#include "boards/crowpanel_1_28.h"
#elif defined(BOARD_WAVESHARE_KNOB_1_8)
#include "boards/waveshare_knob_1_8.h"
#else
#error "No board selected -- build with a PlatformIO env from platformio.ini (e.g. pio run -e crowpanel-1_28-rotary)"
#endif

// The grid every screen is designed on, whatever the panel (ui_scale.h).
#define UI_RES 240
