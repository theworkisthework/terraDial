#include "ui_brand.h"
#include "ui_scale.h"
#include "logo_draw.h"
#include "palette.h"
#include "branding.h"

namespace
{
    lv_obj_t *overlay = nullptr;
    lv_timer_t *splashTimer = nullptr;

    // Unhurried: it's a pen drawing, not a loading bar.
    const uint32_t DRAW_MS = 6400;
    const uint32_t TEXT_FADE_MS = 600;
    // How long the finished mark holds before a boot splash clears itself.
    const uint32_t SPLASH_HOLD_MS = 1800;

    void splashDone(lv_timer_t *)
    {
        splashTimer = nullptr; // a one-shot timer deletes itself after this
        UiBrand::hide();
    }
}

namespace UiBrand
{
    void show()
    {
        if (overlay) return;

        overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(overlay, px(240), px(240));
        lv_obj_center(overlay);
        lv_obj_set_style_bg_color(overlay, Palette::bgApp(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        // Deliberately NOT clickable: dismissal is handled centrally by
        // ScreenSleep so the dismissing touch is swallowed, exactly as it is
        // when waking from sleep. Letting this overlay eat the click itself
        // would mean two different paths doing the same job.
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        // Behind everything else on the top layer: the on-screen status ring
        // (screen_ring.h) and the demo tag stay visible over the logo, as
        // the CrowPanel's LEDs do.
        lv_obj_move_background(overlay);

        // Drawn, not shown: the pen traces the mark in the light text
        // colour, as the bitmap used to appear -- the artwork is a black
        // stroke for print, painted light on the dark face here.
        lv_obj_t *logo = LogoDraw::create(overlay, Palette::text(), DRAW_MS);
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, px(-18));

        // The words wait for the pen, so the eye has one thing to follow.
        lv_obj_t *nameLbl = lv_label_create(overlay);
        lv_label_set_text(nameLbl, Branding::productName());
        lv_obj_set_style_text_font(nameLbl, &UI_FONT_16, 0);
        lv_obj_set_style_text_color(nameLbl, Palette::text(), 0);
        lv_obj_align(nameLbl, LV_ALIGN_CENTER, 0, px(62));
        lv_obj_set_style_opa(nameLbl, LV_OPA_TRANSP, 0);
        lv_obj_fade_in(nameLbl, TEXT_FADE_MS, DRAW_MS);

        lv_obj_t *siteLbl = lv_label_create(overlay);
        lv_label_set_text(siteLbl, Branding::siteLabel());
        lv_obj_set_style_text_font(siteLbl, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(siteLbl, Palette::accent(), 0);
        lv_obj_align(siteLbl, LV_ALIGN_CENTER, 0, px(84));
        lv_obj_set_style_opa(siteLbl, LV_OPA_TRANSP, 0);
        lv_obj_fade_in(siteLbl, TEXT_FADE_MS, DRAW_MS + 200);
    }

    void showSplash()
    {
        if (overlay) return;
        show();
        splashTimer = lv_timer_create(splashDone, DRAW_MS + TEXT_FADE_MS + SPLASH_HOLD_MS, nullptr);
        lv_timer_set_repeat_count(splashTimer, 1);
    }

    void hide()
    {
        if (splashTimer)
        {
            lv_timer_del(splashTimer);
            splashTimer = nullptr;
        }
        if (!overlay) return;
        lv_obj_del(overlay);
        overlay = nullptr;
    }

    bool isShown() { return overlay != nullptr; }
}
