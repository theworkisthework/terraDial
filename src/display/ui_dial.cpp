#include "ui_dial.h"
#include "lucide_icons.h"
#include "palette.h"
#include "radial_ring.h"
#include "ui_widgets.h"
#include "../config/settings.h"
#include "../net/demo_mode.h"
#include <string.h>

// Home: a radial dial -- 8 destinations arranged on a ring around a centre
// hub, per the "TerraPen Dial UI" mockup. The item nearest the top slot is
// largest/brightest; others shrink and fade with angular distance. The ring
// mechanics live in RadialRing (shared with the Jobs screen); this file
// supplies the items, their colours, and the hub.
//
// The spacing is deliberately uneven (RadialRing::setSpread): the top of the
// ring is stretched open and the bottom squeezed shut. Testers liked the
// dial but couldn't read it -- equal chips a uniform 360/count degrees apart
// on a 1.28" panel all look much the same. Spending the angular budget where
// you're looking lets the selection and its two neighbours grow enough to
// tell apart, at the cost of the items you're rotating away from.
//
// That budget is what shrinks as items are added: the ring is a full circle,
// so the even spacing behind the spread is 360/count -- 40 degrees at nine
// items, 36 at ten. The spread absorbs it (the tail bunches tighter while the
// selection keeps its room), but this is the cost of a new dial item, and it
// is why one earns its place rather than being added because it fits.
//
// Job Progress and Alarm Clear are NOT ring items. Both are auto-navigated
// to when the machine enters their state, and both used to carry a dial
// item as well, for the other half of the problem: getting back after
// you've wandered off to change the lights mid-run, since otherwise the
// only route back was to wait for the job to end.
//
// That route now lives on the hub instead. The hub already reports the
// machine state in the middle of the screen -- "RUN", "ALARM" -- so making
// that word the way back costs no ring slot and puts the shortcut on the
// one control that is already telling you the shortcut is relevant. Which
// screen it opens is ui_nav's call, not this file's.
//
// The slots that bought back are why E-Stop is findable: eight items sit 45
// degrees apart where ten sat 36, and E-Stop is a chip you have to land on
// while something is going wrong.
namespace
{
    struct DialItem
    {
        const char *label;
        const char *icon; // a LUCIDE_* glyph (lucide_icons.h)
        // Pinned to alert red regardless of ring position, instead of
        // fading between the raised surface and the accent like every other
        // item. E-Stop has to be findable at a glance mid-panic -- if it
        // only turned red once rotated to the top, you'd be hunting for it
        // exactly when you can least afford to.
        bool alwaysAlert;
    };

    // Ordered by how a session actually runs, not by category: you home,
    // you jog to the work, you set the pen, then you pick a job. The ring
    // rests on item 0, so the first thing under the selection when the
    // dial comes up is the first thing you do. Setup-and-go items lead;
    // the two you reach for when something is wrong (E-Stop, Alarm) sit
    // mid-ring where the alert-red chip is easy to find, and the two you
    // rarely touch mid-job trail at the end.
    const DialItem DIAL_ITEMS[] = {
        {"Home XY", LUCIDE_HOUSE, false},
        {"Jog", LUCIDE_MOVE, false},
        {"Pen", LUCIDE_PEN, false},
        // An SD card, not a folder: inside Jobs, folders are chips of their
        // own, and the same picture shouldn't mean both "this screen" and
        // "a folder on it". Lucide has no SD card; card-sim is the shape.
        {"Jobs", LUCIDE_CARD_SIM, false},
        // Directly after Jobs because that is where it falls in a session:
        // you run the plot, watch it finish, then park to photograph it.
        {"Photo", LUCIDE_CAMERA, false},
        {"E-Stop", LUCIDE_OCTAGON_X, true},
        {"Lights", LUCIDE_LIGHTBULB, false},
        {"Settings", LUCIDE_SETTINGS, false},
    };
    const int DIAL_ITEM_COUNT = 8;
    const int LIGHTS_ITEM = 6;

    // Lights is only on the ring while terraPixel is switched on in
    // Settings > Machine (or demo mode is showing off simulated lights).
    // Deliberately the setting and not terraPixel's reachability: a ring
    // that gained and lost an item as the lights dropped on and off the
    // network would reshuffle under the knob, and would hide the Lights
    // screen exactly when its status is what you need to see.
    bool lightsWanted() { return Config::get().terraPixelEnabled || Demo::isOn(); }

    // Ring position -> DIAL_ITEMS index for the items actually on the ring.
    // DIAL_ITEMS indices are what the rest of the firmware speaks (ui_nav's
    // screen switch, the demo tour), so they stay fixed whatever is shown;
    // only this mapping changes.
    int activeItems[DIAL_ITEM_COUNT];
    int activeCount = 0;
    bool lightsShown = false;

    // Holds the selected item's name + machine status. Sized so the widest
    // status word, "CONNECTING", fits across the hub at the status line's
    // height -- at 82px it was clipped by the hub's curve.
    const lv_coord_t HUB_SIZE = 96;

    // Ring geometry. The ring sits out near the glass so the face is dial,
    // not margin: the selected chip is centred in the band between the
    // hub's rim (radius 48) and the glass (~120), ~5px clear of each. The
    // spread is gentle: at rest the selected chip is 62px, its neighbours
    // ~53px and the bottom one 34px, with gaps of 10-20px all the way
    // round. Size still says which chip is selected, together with the
    // accent colour.
    //
    // A harder spread (it was 0.55, with 24px far chips) made the top
    // three chips legible at the cost of bunching the rest into a
    // cluster at the bottom and leaving the band around it empty.
    const lv_coord_t RING_RADIUS = 84;
    const lv_coord_t RING_SIZE_NEAR = 62;
    const lv_coord_t RING_SIZE_FAR = 34;
    const lv_opa_t RING_OPA_FAR = 100;
    const float RING_SPREAD = 0.3f;

    RadialRing ring;
    lv_obj_t *statusLbl = nullptr;
    bool statusPulsing = false;
    lv_obj_t *nameLbl = nullptr;
    lv_obj_t *iconObjs[DIAL_ITEM_COUNT] = {nullptr};

    // Which icon size each item is currently drawn at -- see uiRingIconSize.
    UiRingIconSize iconSize[DIAL_ITEM_COUNT] = {UiRingIconSmall};

    void updateNameLabel()
    {
        lv_label_set_text(nameLbl, DIAL_ITEMS[activeItems[ring.selectedIndex()]].label);
    }

    void onSelect(int) { updateNameLabel(); }

    void onItemStyle(lv_obj_t *card, int pos, float nearness)
    {
        int i = activeItems[pos]; // the ring speaks positions; the arrays below are by item
        // Card fades from the raised navy surface up to the red accent as it
        // approaches the top slot; its icon fades from muted to full white
        // so the selected item is unmistakable. E-Stop opts out of both the
        // colour blend and the distance fade -- a dimmed E-Stop would defeat
        // the point of pinning its colour.
        lv_opa_t mix = (lv_opa_t)(255 * nearness);
        bool alert = DIAL_ITEMS[i].alwaysAlert;

        if (alert) lv_obj_set_style_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card,
                                  alert ? Palette::alert()
                                        : lv_color_mix(Palette::accent(), Palette::bgSecondary(), mix),
                                  0);

        lv_color_t iconColor = alert ? Palette::accentFg()
                                     : lv_color_mix(Palette::accentFg(), Palette::textMuted(), mix);

        UiRingIconSize want = uiRingIconSize(nearness, iconSize[i]);

        lv_obj_set_style_text_color(iconObjs[i], iconColor, 0);
        // A font change forces a label relayout, unlike the plain colour
        // write above -- skip it unless the size bucket actually flipped,
        // since this runs for every item on every animation frame.
        if (want != iconSize[i])
        {
            iconSize[i] = want;
            lv_obj_set_style_text_font(iconObjs[i], uiRingIconFont(want), 0);
        }
    }

    lv_obj_t *makeCard(lv_obj_t *parent, int index)
    {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 12, 0);
        lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(card, 0, 0);
        // Extends the touch area beyond the drawn circle without changing
        // how it looks -- the shrunken off-top cards are much smaller than a
        // fingertip.
        lv_obj_set_ext_click_area(card, 10);

        {
            lv_obj_t *iconLbl = lv_label_create(card);
            lv_label_set_text(iconLbl, DIAL_ITEMS[index].icon);
            // Must match iconSize[]'s initial value: onItemStyle only writes
            // a font when the bucket CHANGES, so a mismatch here would leave
            // far items drawn at the wrong size until they happened to pass
            // through another bucket.
            lv_obj_set_style_text_font(iconLbl, uiRingIconFont(UiRingIconSmall), 0);
            lv_obj_center(iconLbl);
            iconObjs[index] = iconLbl;
        }
        return card;
    }

    // Supplied by ui_nav. Returns true if the machine's current state had a
    // screen worth jumping to, in which case the tap meant that rather than
    // "open the selected item".
    bool (*onStatusTap)() = nullptr;

    // Supplied by ui_nav, called with a DIAL_ITEMS index.
    void (*onOpenItem)(int index) = nullptr;
    void onRingOpen(int pos)
    {
        if (onOpenItem) onOpenItem(activeItems[pos]);
    }

    lv_obj_t *dialScreen = nullptr;
    lv_obj_t *hubObj = nullptr;

    // (Re)builds the ring from scratch to match lightsWanted(). Cards can't
    // just be hidden: RadialRing lays out every item it holds, so a hidden
    // one would leave a gap. Keeps the selection on the same item where it
    // still exists -- this runs as you leave Settings, and you should come
    // back to the Settings chip, not wherever position 0 now is.
    void buildRing()
    {
        int keepItem = activeCount ? activeItems[ring.selectedIndex()] : 0;
        lightsShown = lightsWanted();
        ring.clear();
        activeCount = 0;
        for (int i = 0; i < DIAL_ITEM_COUNT; i++)
        {
            if (i == LIGHTS_ITEM && !lightsShown) continue;
            iconSize[i] = UiRingIconSmall; // makeCard draws at Small; see there
            activeItems[activeCount++] = i;
            ring.addItem(makeCard(dialScreen, i));
        }
        // Cards are created after the hub, so raise it back above them.
        lv_obj_move_foreground(hubObj);
        for (int pos = 0; pos < activeCount; pos++)
        {
            if (activeItems[pos] != keepItem) continue;
            for (int s = 0; s < pos; s++) ring.selectNext();
            break;
        }
        updateNameLabel();
    }

    void hubTapCb(lv_event_t *e)
    {
        (void)e;
        // State first. When the hub is reading RUN or ALARM, that is what
        // the middle of the screen is about, and it is what a tap on it
        // means -- the selected ring item is still one tap away on its own
        // chip. When there's nothing going on, the hub goes back to being a
        // big central shortcut to whatever is selected.
        if (onStatusTap && onStatusTap()) return;
        ring.openSelected();
    }

    void statusPulseCb(void *obj, int32_t v)
    {
        lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    }

    // A slow breathe on the status word while the panel is still trying to
    // reach the plotter, so "CONNECTING" reads as something in progress
    // rather than a state it has settled into.
    void setStatusPulsing(bool on)
    {
        if (on == statusPulsing) return;
        statusPulsing = on;
        lv_anim_del(statusLbl, statusPulseCb);
        if (!on)
        {
            lv_obj_set_style_text_opa(statusLbl, LV_OPA_COVER, 0);
            return;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, statusLbl);
        lv_anim_set_exec_cb(&a, statusPulseCb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
        lv_anim_set_time(&a, 900);
        lv_anim_set_playback_time(&a, 900);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    const char *textForMode(MachineMode mode)
    {
        switch (mode)
        {
            case MachineMode::Run:    return "RUN";
            case MachineMode::Hold:   return "HOLD";
            case MachineMode::Alarm:  return "ALARM";
            case MachineMode::Homing: return "HOMING";
            case MachineMode::Done:   return "DONE";
            case MachineMode::Boot:   return "CONNECTING";
            case MachineMode::Idle:
            default:                  return "IDLE";
        }
    }

    // Machine-state colours, pulled toward the terraForge palette so the hub
    // doesn't read as a set of unrelated primaries against the navy. Run/
    // Done stay green-ish and Hold stays amber because those meanings are
    // near-universal on CNC gear and worth keeping literal.
    lv_color_t colorForMode(MachineMode mode)
    {
        switch (mode)
        {
            case MachineMode::Run:    return lv_color_hex(0x3ddc84);
            case MachineMode::Hold:   return lv_color_hex(0xffa726);
            case MachineMode::Alarm:  return Palette::accent();          // the terraForge red
            case MachineMode::Homing: return Palette::accentSecondary(); // file-browser blue
            case MachineMode::Done:   return lv_color_hex(0x3ddc84);
            case MachineMode::Boot:   return Palette::textFaint();
            case MachineMode::Idle:
            default:                  return Palette::textMuted();
        }
    }
}

lv_obj_t *uiDialCreate()
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, Palette::bgApp(), 0);

    // Centre hub -- same circular styling as the ring items, just bigger and
    // stationary. Holds the selected item's name (so rotating never hides
    // it behind the top card) and the live machine status.
    lv_obj_t *hub = lv_obj_create(scr);
    dialScreen = scr;
    hubObj = hub;
    lv_obj_set_size(hub, HUB_SIZE, HUB_SIZE);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, Palette::bgSecondary(), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hub, Palette::border(), 0);
    lv_obj_set_style_border_width(hub, 1, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    // The hub names the selected item, so tapping it opens that item -- a
    // big, central, always-in-the-same-place target, unlike the ring cards
    // which move as you rotate.
    lv_obj_add_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hub, hubTapCb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

    nameLbl = lv_label_create(hub);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nameLbl, Palette::text(), 0);
    lv_obj_align(nameLbl, LV_ALIGN_CENTER, 0, -8);

    statusLbl = lv_label_create(hub);
    lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_12, 0);
    // Pinned to the width the round hub has at this height, so a status
    // too long to fit scrolls within it instead of being cut off by the
    // hub's curve. Every current word fits; this is the safety net.
    lv_obj_set_width(statusLbl, HUB_SIZE - 12);
    lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(statusLbl, LV_ALIGN_CENTER, 0, 12);

    ring.create(scr, RING_RADIUS, RING_SIZE_NEAR, RING_SIZE_FAR, LV_OPA_COVER, RING_OPA_FAR);
    ring.setSpread(RING_SPREAD);
    ring.setOnItemStyle(onItemStyle);
    ring.setOnSelect(onSelect);
    ring.setOnOpen(onRingOpen);
    buildRing();
    return scr;
}

void uiDialSetHandlers(void (*onOpen)(int index), bool (*onStatus)())
{
    onOpenItem = onOpen;
    onStatusTap = onStatus;
}

void uiDialSelectNext() { ring.selectNext(); }
void uiDialSelectPrev() { ring.selectPrev(); }
void uiDialOpenSelected() { ring.openSelected(); }
int uiDialSelectedIndex() { return activeItems[ring.selectedIndex()]; }

void uiDialUpdate(const FluidNCStatus &st)
{
    // Lights comes and goes with Settings > Machine's switch and with demo
    // mode, both only changed from the Settings screen -- so this rebuild
    // always happens out of sight, before you're back on the dial.
    if (lightsWanted() != lightsShown) buildRing();

    // A live job says PROGRESS rather than RUN, because on a live job this
    // label is a button: it is the hub tap that opens the Job Progress
    // screen (see the note at the top of this file), and a control should
    // name where it goes rather than restate what you can already see from
    // the machine itself. "RUN" was also the one state where the hub said
    // the least -- you can hear the plotter running.
    //
    // The colour still tracks the real mode, so a paused job reads as an
    // amber PROGRESS: where the tap goes hasn't changed, but the machine
    // isn't moving and the ring and the hub should both say so.
    // Only on a change: setting a scrolling label's text restarts its
    // scroll, and this runs on every status update.
    const char *text = st.jobActive ? "PROGRESS" : textForMode(st.mode);
    if (strcmp(lv_label_get_text(statusLbl), text) != 0) lv_label_set_text(statusLbl, text);
    lv_obj_set_style_text_color(statusLbl, colorForMode(st.mode), 0);
    setStatusPulsing(!st.jobActive && st.mode == MachineMode::Boot);
}
