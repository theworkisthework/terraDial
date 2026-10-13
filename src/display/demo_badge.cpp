#include "demo_badge.h"
#include "ui_scale.h"
#include <lvgl.h>
#include "palette.h"
#include "../net/demo_mode.h"

namespace
{
    lv_obj_t *ring = nullptr;
    lv_obj_t *tag = nullptr;
    bool shown = false;

    // Tapping the tag leaves demo mode. Without it the only way out was the
    // switch on Settings > About -- several screens deep, while the tour
    // keeps taking the panel back every 20s -- or a restart.
    void tagTapCb(lv_event_t *)
    {
        Demo::set(false);
    }

    void create()
    {
#if BOARD_HAS_SCREEN_RING
        // No outline on a board whose status ring is drawn round the edge
        // of the display (screen_ring.h): the outline sat right on top of
        // it and hid the homing sweep, the alarm flash and the rest. The
        // tag alone says "demo" clearly enough.
        const lv_coord_t TAG_Y = px(12); // just inside the status ring's dots
#else
        const lv_coord_t TAG_Y = px(3);

        // Just inside the bezel, where no screen draws anything: every
        // screen keeps its content ~5px or more clear of the glass edge.
        ring = lv_obj_create(lv_layer_top());
        lv_obj_set_size(ring, px(240), px(240));
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, Palette::accentSecondary(), 0);
        lv_obj_set_style_border_width(ring, px(3), 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        // lv_obj_create()'s objects take clicks by default; a full-screen
        // one left clickable would swallow every touch on every screen.
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
#endif

        tag = lv_label_create(lv_layer_top());
        lv_label_set_text(tag, "DEMO");
        lv_obj_set_style_text_font(tag, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(tag, Palette::bgApp(), 0);
        lv_obj_set_style_text_letter_space(tag, px(1), 0);
        lv_obj_set_style_bg_color(tag, Palette::accentSecondary(), 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tag, px(8), 0);
        lv_obj_set_style_pad_hor(tag, px(7), 0);
        lv_obj_set_style_pad_ver(tag, px(1), 0);
        lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, TAG_Y);
        // The tag is small and hard against the bezel, so its touch target
        // reaches well past what's drawn. Nothing else lives up there.
        lv_obj_add_flag(tag, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(tag, px(12));
        lv_obj_add_event_cb(tag, tagTapCb, LV_EVENT_CLICKED, NULL);
    }
}

namespace DemoBadge
{
    void update()
    {
        bool on = Demo::isOn();
        if (on == shown) return;
        shown = on;
        if (!tag) create();
        if (on)
        {
            if (ring) lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tag, LV_OBJ_FLAG_HIDDEN);
            // Above anything created on the top layer since they last showed.
            if (ring) lv_obj_move_foreground(ring);
            lv_obj_move_foreground(tag);
        }
        else
        {
            if (ring) lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
