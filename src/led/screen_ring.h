#pragma once

// An LED ring drawn on the display, for boards without a real one
// (BOARD_HAS_SCREEN_RING in pins.h). It has exactly the slice of
// Adafruit_NeoPixel's interface that panel_ring.cpp uses, so PanelRing's
// animations -- homing sweep, alarm flash, jog chase, plot breathing --
// run unchanged on either. Only panel_ring.cpp should include this.
//
// The "LEDs" are LED_RING_COUNT small dots just inside the edge of the
// round panel, on lv_layer_top() so they sit above every screen, and
// non-clickable so touches pass straight through to whatever's beneath.
// Each dot is its own small object, so a frame only redraws the dots whose
// colour actually changed -- never the whole face, which is what one big
// arc would invalidate on every step of a pulse.
//
// Main loop only, like the rest of LVGL (PanelRing::update() runs there).

#include <lvgl.h>
#include <math.h>
#include "pins.h"
#include "ui_scale.h"

class ScreenRing
{
public:
    explicit ScreenRing(uint16_t count) : count_(count) {}

    void begin()
    {
        // Dots sit on a circle just inside the face's edge, which on a round
        // panel is the bezel line -- the same place the CrowPanel's LEDs
        // glow from. Clear of every screen's content, which keeps inside
        // ~94px of centre on the 240 grid (see ui_widgets.h).
        const float radius = PANEL_RES / 2.0f - pxf(5);
        for (uint16_t i = 0; i < count_; i++)
        {
            lv_obj_t *dot = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

            // Index 0 at 12 o'clock, ascending COUNTER-clockwise -- the same
            // order as the CrowPanel's physical strip, which panel_ring.cpp's
            // chase direction is written for.
            float a = -2.0f * (float)M_PI * i / count_;
            lv_obj_align(dot, LV_ALIGN_CENTER,
                         (lv_coord_t)lroundf(radius * sinf(a)),
                         (lv_coord_t)lroundf(-radius * cosf(a)));
            dots_[i] = dot;
        }
    }

    void setBrightness(uint8_t b) { brightness_ = b; }
    void clear() { for (uint16_t i = 0; i < count_; i++) rgb_[i] = 0; }

    void setPixelColor(uint16_t i, uint8_t r, uint8_t g, uint8_t b)
    {
        if (i < count_) rgb_[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    void show()
    {
        for (uint16_t i = 0; i < count_; i++)
        {
            if (!dots_[i]) return; // begin() not run yet

            // An LED's colour is mostly hue; how much it gives off is its
            // brightest channel times the strip brightness. On screen that
            // splits naturally: hue as the dot's colour (normalised so the
            // brightest channel is full), output as its opacity over
            // whatever's behind. A dim amber LED becomes a faint amber dot,
            // not a dark brown one, and off is simply not there.
            uint32_t c = rgb_[i];
            uint8_t r = c >> 16, g = c >> 8, b = c;
            uint8_t peak = r > g ? (r > b ? r : b) : (g > b ? g : b);
            lv_opa_t opa = (lv_opa_t)(((uint32_t)peak * brightness_) / 255);
            lv_color_t col = peak ? lv_color_make((uint32_t)r * 255 / peak, (uint32_t)g * 255 / peak, (uint32_t)b * 255 / peak)
                                  : lv_color_black();

            // Only touch the object on a real change: every style write
            // invalidates the dot, and most frames most dots are unchanged.
            uint32_t key = ((uint32_t)lv_color_to16(col) << 8) | opa;
            if (key == shown_[i]) continue;
            shown_[i] = key;
            lv_obj_set_style_bg_color(dots_[i], col, 0);
            lv_obj_set_style_bg_opa(dots_[i], opa, 0);
        }
    }

private:
    static const uint16_t MAX_COUNT = 64;
    static constexpr lv_coord_t DOT_SIZE = px(5);

    uint16_t count_;
    uint8_t brightness_ = 255;
    uint32_t rgb_[MAX_COUNT] = {0};
    uint32_t shown_[MAX_COUNT] = {0};
    lv_obj_t *dots_[MAX_COUNT] = {nullptr};
};
