#include "ui_pen.h"
#include "lucide_icons.h"
#include "../net/fluidnc_client.h"
#include "../config/settings.h"
#include "palette.h"
#include "ui_screen_shell.h"

namespace
{
    // FluidNC doesn't report the servo's actual angle in its status
    // stream, so this is purely an optimistic UI-tracked assumption --
    // it can drift from the real physical state if the machine loses
    // power or is jogged by another client. Default to "up" as the
    // safer assumption at boot (a pen that's actually down when we think
    // it's up just means the next drag looks like a no-op; the reverse
    // could scratch the bed on the first jog).
    bool penIsUp = true;

    lv_obj_t *upSeg = nullptr;
    lv_obj_t *downSeg = nullptr;
    lv_obj_t *upLbl = nullptr;
    lv_obj_t *downLbl = nullptr;

    void restyleSegments()
    {
        lv_obj_set_style_bg_color(upSeg, penIsUp ? Palette::accent() : Palette::bgSecondary(), 0);
        lv_obj_set_style_text_color(upLbl, penIsUp ? Palette::accentFg() : Palette::textMuted(), 0);
        lv_obj_set_style_bg_color(downSeg, !penIsUp ? Palette::accent() : Palette::bgSecondary(), 0);
        lv_obj_set_style_text_color(downLbl, !penIsUp ? Palette::accentFg() : Palette::textMuted(), 0);
    }

    void setPenUp(bool up)
    {
        // A pen command isn't a $J= jog, so FluidNC won't refuse it mid-plot
        // the way it did the old relative jog -- it would land in the middle
        // of the job's own pen moves. Refuse it here instead. Not on plain
        // Run: FluidNC reports that for our own pen move too, and up-then-
        // down in quick succession should just queue.
        const FluidNCStatus &st = fluidNC.status();
        if (st.jobActive || st.mode == MachineMode::Hold) return;

        // No "already in that state" early-out: the commands are absolute
        // (Settings > Machine), so re-sending one is harmless -- and tapping
        // the lit segment again is how you resync after the machine was moved
        // from elsewhere, or after the up/down commands were swapped.
        const AppSettings &cfg = Config::get();
        const char *cmd = up ? cfg.penUpCmd : cfg.penDownCmd;
        if (!cmd[0]) return; // cleared in Settings -- nothing to send
        penIsUp = up;
        fluidNC.sendGcodeLine(cmd);
        restyleSegments();
    }

    void upSegCb(lv_event_t *e) { (void)e; setPenUp(true); }
    void downSegCb(lv_event_t *e) { (void)e; setPenUp(false); }
}

lv_obj_t *uiPenCreate()
{
    ScreenShell shell = createScreenShell("PEN", LUCIDE_PEN);

    // Segmented control (not a toggle switch): both states are always
    // visible with the active one accent-filled, so the current state
    // reads at a glance rather than needing to infer it from a single
    // button's label.
    lv_obj_t *seg = lv_obj_create(shell.content);
    lv_obj_set_size(seg, 140, 64); // was 170x40 -- taller for an easier touch target (170 also overflowed the shell's ~142px-wide safe content area)
    lv_obj_set_style_bg_color(seg, Palette::bgPanel(), 0);
    lv_obj_set_style_radius(seg, 20, 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_set_style_pad_all(seg, 3, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    upSeg = lv_obj_create(seg);
    lv_obj_set_size(upSeg, lv_pct(48), lv_pct(100));
    lv_obj_set_style_radius(upSeg, 17, 0);
    lv_obj_set_style_border_width(upSeg, 0, 0);
    lv_obj_clear_flag(upSeg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(upSeg, upSegCb, LV_EVENT_CLICKED, NULL);
    upLbl = lv_label_create(upSeg);
    lv_label_set_text(upLbl, "Pen\nup");
    lv_obj_set_style_text_align(upLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(upLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(upLbl);

    downSeg = lv_obj_create(seg);
    lv_obj_set_size(downSeg, lv_pct(48), lv_pct(100));
    lv_obj_set_style_radius(downSeg, 17, 0);
    lv_obj_set_style_border_width(downSeg, 0, 0);
    lv_obj_clear_flag(downSeg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(downSeg, downSegCb, LV_EVENT_CLICKED, NULL);
    downLbl = lv_label_create(downSeg);
    lv_label_set_text(downLbl, "Pen\ndown");
    lv_obj_set_style_text_align(downLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(downLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(downLbl);

    restyleSegments();
    return shell.screen;
}

void uiPenToggle() { setPenUp(!penIsUp); }
bool uiPenIsDown() { return !penIsUp; }
