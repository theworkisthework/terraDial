#pragma once
#include <lvgl.h>
#include "ui_scale.h"

// The home dial's rotating ring, as a reusable widget.
//
// Items sit around a circle; rotating steps the ring with a springy
// overshoot so the selected item settles at the top slot. Items shrink and
// fade continuously with angular distance from that slot, so the selection
// is obvious without any separate highlight. Spacing is even by default and
// can be biased toward the top slot with setSpread().
//
// Extracted from ui_dial.cpp so the Jobs screen can present SD files the
// same way instead of inventing a second browsing idiom. The widget owns
// geometry, rotation and the animation; the owning screen builds each
// item's contents and colours it via setOnItemStyle().
class RadialRing
{
public:
    // Items a non-virtual ring can hold, and the largest chip pool a virtual
    // one can have. A virtual ring's list length is unlimited.
    static const int MAX_ITEMS = 40;

    // radius/sizes are in px on the 240x240 panel. The defaults are the
    // original evenly-spaced tuning; every screen now overrides them, since
    // each has a different hub size to clear and its own spread to suit.
    void create(lv_obj_t *parent,
                lv_coord_t radius = px(74),
                lv_coord_t sizeNear = px(62),
                lv_coord_t sizeFar = px(30),
                lv_opa_t opaNear = LV_OPA_COVER,
                lv_opa_t opaFar = 110);

    // Switches from "spread N items evenly around the full circle" to an
    // open arc: items sit at a FIXED angular pitch and anything beyond
    // halfArcDeg from the top slot is hidden entirely.
    //
    // Two reasons the Jobs screen needs this. It leaves the bottom of the
    // dial empty, so the back button there can't be confused for an item --
    // a mis-tap that starts a plot is not a cheap mistake. And because the
    // list no longer has to fit in 360 degrees, it scrolls instead of
    // cramming: 40 files on a full circle would be 9 degrees apart, whereas
    // an arc shows a readable handful and slides the rest through.
    //
    // Arc mode is a linear list, not a loop -- selection clamps at both ends
    // rather than wrapping, which is what a file list should do.
    void setArcLayout(float stepDeg, float halfArcDeg);

    // Bends the even angular spacing so slots near the top slot are pushed
    // apart and slots near the bottom bunch together -- the ring reads like
    // a perspective view rather than a flat circle.
    //
    // Why: on a 1.28" panel eight evenly spread chips are all roughly the
    // same size and 45 degrees apart, and users reported they couldn't tell
    // at a glance which one was selected or what its neighbours were.
    // Spending the ring's angular budget where the eye actually is -- the
    // top -- buys the selection and its immediate neighbours room to grow,
    // and costs nothing but legibility of the items you're rotating away
    // from anyway.
    //
    // `amount` is 0 for the plain even circle up to just under 1 for the
    // most extreme bend; values >= 1 make the mapping non-monotonic (items
    // would swap order), so it is clamped.
    void setSpread(float amount);

    // Registers a caller-built chip. It MUST be a child of the same parent
    // passed to create(), since placement uses lv_obj_align against it.
    void addItem(lv_obj_t *item);
    void clear();

    // Virtual mode, for lists too long to give every entry its own chip
    // (the Jobs screen's SD folders). Arc layout only: the ring builds
    // `poolSize` chips with createItem() once, and as the list scrolls it
    // hands each chip to whichever entry has come into view, calling
    // bindItem(chip, index) so the owner can dress it for that entry. A
    // chip keeps its entry for as long as the entry stays on screen, so
    // binds happen only as entries enter.
    //
    // poolSize must cover everything the arc can show at once:
    // floor(2 * halfArcDeg / stepDeg) + 1. Call after setArcLayout().
    void setVirtual(int poolSize, lv_obj_t *(*createItem)(lv_obj_t *parent),
                    void (*bindItem)(lv_obj_t *item, int index));
    // Virtual mode: the list's length. Resets the selection to the first
    // entry and rebinds every chip, since the entries behind the indices
    // have changed.
    void setCount(int count);

    void selectNext();
    void selectPrev();
    int selectedIndex() const { return selectedIndex_; }
    int count() const { return count_; }

    // Fires the open callback for the current selection (knob click).
    void openSelected();
    void setOnOpen(void (*cb)(int index)) { onOpen_ = cb; }

    // Fires whenever the selection changes -- for updating a centre hub or
    // any other out-of-ring display.
    void setOnSelect(void (*cb)(int index)) { onSelect_ = cb; }

    // Called for every item on every layout pass. `nearness` is 1.0 at the
    // top slot and 0.0 diametrically opposite, so callers can blend colour,
    // swap icon sizes, etc. Runs AFTER the ring applies its own size and
    // opacity, so a caller may override either. In virtual mode `index` is
    // the list entry the chip currently shows, and the same chip turns up
    // under different indices over time -- keep per-chip state on the chip.
    void setOnItemStyle(void (*cb)(lv_obj_t *item, int index, float nearness)) { onItemStyle_ = cb; }

    // Re-applies layout/styling without moving the ring (e.g. after the
    // owner changes an item's contents).
    void relayout() { layout(); }

    // Hides/shows the whole ring without destroying it -- for screens that
    // swap the ring out for a detail view and back (Settings).
    void setVisible(bool visible);

private:
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *items_[MAX_ITEMS] = {nullptr};
    float nearness_[MAX_ITEMS] = {0.0f}; // per-item, from the last layout pass -- used to stack them
    // Logical list length. Equal to the number of chips, except in virtual
    // mode, where the chips are the pool and this is the list.
    int count_ = 0;

    bool virtual_ = false;
    int poolCount_ = 0;
    int slotIndex_[MAX_ITEMS] = {0}; // virtual: the entry each chip shows, -1 = unbound
    void (*bindItem_)(lv_obj_t *item, int index) = nullptr;
    int objCount() const { return virtual_ ? poolCount_ : count_; }
    // The entry chip `slot` shows (identity outside virtual mode).
    int indexOfSlot(int slot) const { return virtual_ ? slotIndex_[slot] : slot; }
    void assignSlots(float offsetDeg);
    int selectedIndex_ = 0;

    lv_coord_t radius_ = px(74);
    lv_coord_t sizeNear_ = px(62);
    lv_coord_t sizeFar_ = px(30);
    lv_opa_t opaNear_ = LV_OPA_COVER;
    lv_opa_t opaFar_ = 110;

    int32_t offsetX100_ = 0; // ring rotation as currently rendered (moves mid-animation)
    // Last commanded rest position -- always an exact multiple of the step
    // angle. New targets are always computed from THIS, never from
    // offsetX100_: that one can be caught mid-animation (spin the knob fast
    // enough to interrupt an in-flight spring) and computing the next target
    // from an unsettled value permanently knocks the ring off-grid from
    // selectedIndex_ -- i.e. the top item stops matching what opens.
    int32_t targetOffsetX100_ = 0;

    void (*onOpen_)(int index) = nullptr;
    void (*onSelect_)(int index) = nullptr;
    void (*onItemStyle_)(lv_obj_t *item, int index, float nearness) = nullptr;

    // >0 enables arc mode (see setArcLayout); 0 means spread evenly around
    // the full circle.
    float fixedStepDeg_ = 0.0f;
    float halfArcDeg_ = 180.0f;

    bool visible_ = true;

    // See setSpread(). 0 = even spacing.
    float spread_ = 0.0f;

    bool arcMode() const { return fixedStepDeg_ > 0.0f; }
    float stepDeg() const
    {
        if (arcMode()) return fixedStepDeg_;
        return count_ > 0 ? 360.0f / count_ : 360.0f;
    }
    // Maps an evenly-spaced angle to its spread position. Identity when
    // spread_ is 0.
    float spreadAngle(float angle, float limit) const;
    void layout();
    void animateTo(int32_t targetX100);
    void stepSelection(int delta);

    static void animCb(void *inst, int32_t v);
    static void itemTapCb(lv_event_t *e);
};
