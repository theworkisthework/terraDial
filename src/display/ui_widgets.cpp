#include "ui_widgets.h"
#include "ui_scale.h"
#include "lucide_icons.h"
#include "palette.h"

lv_obj_t *uiMakeRow(lv_obj_t *parent, const char *labelText, lv_obj_t **outLabel)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, px(2), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    // A row is a passive container -- it must never scroll or draw a
    // scrollbar. Left scrollable, any child a few px wider than the row
    // (e.g. a line of chips) tips it into horizontal overflow, and LVGL
    // then draws a translucent grey scrollbar straight across that child.
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    if (labelText)
    {
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, labelText);
        lv_obj_set_style_text_font(lbl, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(lbl, Palette::textMuted(), 0);
        if (outLabel) *outLabel = lbl;
    }

    return row;
}

lv_obj_t *uiMakePanel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, px(240), px(240));
    lv_obj_center(panel);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0); // the screen behind already carries the background
    lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_hor(panel, px(30), 0);
    lv_obj_set_style_pad_top(panel, px(46), 0);
    lv_obj_set_style_pad_bottom(panel, px(56), 0); // clears the back button
    lv_obj_set_style_pad_row(panel, px(8), 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // No scrollbar: a straight bar down the edge of a CIRCULAR panel can't
    // hug anything, it just cuts across the face and reads as a rendering
    // fault. Scrolling still works by knob and by drag.
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

    if (title)
    {
        lv_obj_t *titleLbl = lv_label_create(panel);
        lv_label_set_text(titleLbl, title);
        lv_obj_set_style_text_font(titleLbl, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(titleLbl, Palette::accentSecondary(), 0);
    }
    return panel;
}

namespace
{
    const lv_coord_t SLIDER_TRACK_H = px(10); // chunkier than stock -- easier to grab on a small round panel
    const lv_coord_t SLIDER_KNOB_PAD = px(4);
    const lv_coord_t SLIDER_KNOB_R = SLIDER_TRACK_H / 2 + SLIDER_KNOB_PAD;
    const lv_coord_t SLIDER_TRACK_RADIUS = SLIDER_TRACK_H / 2;

    // Fills the left end of the track that the MAIN padding (see
    // uiMakeSlider) leaves out of the indicator. Runs once the track has
    // been drawn and before the indicator and knob are, so both land on top
    // of it. The cap runs a whole knob width: at min the knob hides all of
    // it, and past that its straight middle covers the join with the
    // indicator's rounded start, so the two read as one bar.
    void sliderCapCb(lv_event_t *e)
    {
        lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
        // MAIN also reports a post-draw border pass; only the track counts.
        if (dsc->part != LV_PART_MAIN || dsc->class_p != &lv_obj_class ||
            dsc->type != LV_OBJ_DRAW_PART_RECTANGLE)
            return;

        lv_obj_t *slider = lv_event_get_target(e);
        lv_area_t cap;
        lv_obj_get_coords(slider, &cap);
        cap.x2 = cap.x1 + 2 * SLIDER_KNOB_R - 1;

        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR);
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = SLIDER_TRACK_RADIUS;
        lv_draw_rect(dsc->draw_ctx, &rect, &cap);
    }
}

lv_obj_t *uiMakeSlider(lv_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    // The knob is centred on the end of the indicator. Left to itself that
    // runs from the track's very first pixel to its very last, so at min and
    // max half the knob hangs past the track -- clipped flat by the row it
    // sits in, and once that was fixed by insetting the track, the bar no
    // longer lined up with the left-aligned rows around it.
    //
    // Instead the track's MAIN padding stops the indicator one knob radius
    // short of each end. LVGL keys the knob's travel and the drag-to-value
    // mapping off that same padded range, so the knob's edge stops exactly
    // at the track's ends and the track can fill the full column. The
    // indicator is also clipped to the padded range, which would leave an
    // unfilled stub at the left; sliderCapCb paints it.
    //
    // The wrapper only gives the knob room vertically, where it still
    // overhangs the track by SLIDER_KNOB_PAD.
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_hor(wrap, 0, 0);
    lv_obj_set_style_pad_ver(wrap, SLIDER_KNOB_PAD, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);
    // Touches only reach the slider through the wrapper's own hit area, so
    // without this the slider's 8px ext click area below would be cut to the
    // wrapper's SLIDER_KNOB_PAD. Not clickable itself: a near-miss beside the
    // track still falls through to the slider or the scrolling panel.
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(wrap, px(8) - SLIDER_KNOB_PAD);

    lv_obj_t *slider = lv_slider_create(wrap);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_set_height(slider, SLIDER_TRACK_H);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);

    // Unfilled track
    lv_obj_set_style_bg_color(slider, Palette::bgPanel(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, SLIDER_TRACK_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(slider, SLIDER_KNOB_R, LV_PART_MAIN);
    lv_obj_add_event_cb(slider, sliderCapCb, LV_EVENT_DRAW_PART_END, NULL);
    // Filled portion
    lv_obj_set_style_bg_color(slider, Palette::accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, SLIDER_TRACK_RADIUS, LV_PART_INDICATOR);
    // Handle -- white on the red reads clearly and matches accentFg usage
    lv_obj_set_style_bg_color(slider, Palette::accentFg(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, SLIDER_KNOB_PAD, LV_PART_KNOB);
    // Extends the touch area past the 10px track without drawing bigger.
    lv_obj_set_ext_click_area(slider, px(8));
    return slider;
}

lv_obj_t *uiMakeSwitch(lv_obj_t *parent, bool checked)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, px(44), px(24));
    lv_obj_set_style_bg_color(sw, Palette::bgPanel(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, Palette::accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, Palette::accentFg(), LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, px(8));
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

lv_obj_t *uiMakeButton(lv_obj_t *parent, const char *text, lv_obj_t **outLabel)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, px(34));
    lv_obj_set_style_radius(btn, px(17), 0);
    lv_obj_set_style_bg_color(btn, Palette::accent(), 0);
    lv_obj_set_style_bg_color(btn, Palette::accentHover(), LV_STATE_PRESSED);
    // LV_STATE_DISABLED has to be styled explicitly here, and it carries the
    // whole weight of communicating "not now": LVGL's POINTER indev doesn't
    // gate events on the disabled state the way its keypad one does, so a
    // disabled button still fires CLICKED on a tap. Callers must ignore the
    // event themselves -- this only makes the button stop inviting it.
    lv_obj_set_style_bg_color(btn, Palette::bgSecondary(), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &UI_FONT_12, 0);
    lv_obj_set_style_text_color(lbl, Palette::accentFg(), 0);
    lv_obj_set_style_text_color(lbl, Palette::textFaint(), LV_STATE_DISABLED);
    lv_obj_center(lbl);
    if (outLabel) *outLabel = lbl;
    return btn;
}

lv_obj_t *uiMakeTextField(lv_obj_t *parent, const char *labelText, lv_obj_t **outValue)
{
    lv_obj_t *row = uiMakeRow(parent, labelText);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, Palette::bgPanel(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, Palette::border(), 0);
    lv_obj_set_style_border_width(box, px(1), 0);
    // terraForge's focus:border-accent, shown while the finger is down --
    // the keyboard that opens next covers the field, so press is the only
    // moment a "focused" look could be seen.
    lv_obj_set_style_border_color(box, Palette::accent(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(box, px(8), 0);
    lv_obj_set_style_pad_hor(box, px(10), 0);
    lv_obj_set_style_pad_ver(box, px(6), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *value = lv_label_create(box);
    lv_obj_set_width(value, lv_pct(100));
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(value, &UI_FONT_12, 0);
    lv_obj_set_style_text_color(value, Palette::text(), 0);
    if (outValue) *outValue = value;
    return box;
}

void uiKnobScroll(lv_obj_t *page, int32_t delta)
{
    // Per detent. Was 24 -- about one row of 12px text -- which made a long
    // page like About a lot of turning even before the lost-step bug above
    // is accounted for. 36 is a row and a half: still easy to stop on any
    // given line.
    const lv_coord_t STEP_PX = px(36);
    // LVGL's scroll animations run 200-400ms (SCROLL_ANIM_TIME_MAX in
    // lv_obj_scroll.c). A detent inside that window is continuing the same
    // scroll, so it adds to where that one is headed.
    const uint32_t CONTINUE_MS = 400;

    static lv_obj_t *lastPage = nullptr;
    static lv_coord_t target = 0;
    static uint32_t lastAt = 0;

    if (!page || delta == 0) return;

    lv_coord_t now = lv_obj_get_scroll_y(page);
    bool continuing = page == lastPage && lv_tick_elaps(lastAt) < CONTINUE_MS;
    lv_coord_t to = (continuing ? target : now) + (lv_coord_t)delta * STEP_PX;

    // Clamp to the page, or a spin past the end would bank distance that
    // the next spin back has to unwind before anything moves.
    lv_coord_t maxY = now + lv_obj_get_scroll_bottom(page);
    if (to > maxY) to = maxY;
    if (to < 0) to = 0;

    lastPage = page;
    target = to;
    lastAt = lv_tick_get();
    if (to != now) lv_obj_scroll_to_y(page, to, LV_ANIM_ON);
}

UiRingIconSize uiRingIconSize(float nearness, UiRingIconSize current)
{
    if (nearness > (current >= UiRingIconLarge ? 0.80f : 0.90f)) return UiRingIconLarge;
    if (nearness > (current >= UiRingIconMedium ? 0.42f : 0.52f)) return UiRingIconMedium;
    return UiRingIconSmall;
}

const lv_font_t *uiRingIconFont(UiRingIconSize size)
{
    switch (size)
    {
        // Lucide fonts: the ring icons are LUCIDE_* glyphs (lucide_icons.h).
        case UiRingIconLarge:  return &UI_ICONS_32;
        case UiRingIconMedium: return &UI_ICONS_24;
        case UiRingIconSmall:
        default:               return &UI_ICONS_14;
    }
}
