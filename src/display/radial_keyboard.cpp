#include "radial_keyboard.h"
#include "palette.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace
{
    // ---- key model ----
    // Everything on the rim is a Key. Printable keys append their character;
    // action keys do something to the buffer or the overlay.
    enum class Action : uint8_t
    {
        Char,
        Space,
        Backspace,
        NextPage,
        Reveal,
        Accept,
        Cancel,
    };

    struct Key
    {
        const char *label; // what's drawn on the rim
        char ch;           // the character appended, for Action::Char
        Action action;
    };

    // Three pages, cycled by the "abc/ABC/123" key. Splitting them keeps any
    // one ring sparse enough that neighbouring keys don't collide visually.
    const char *PAGE_CHARS[] = {
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "0123456789.-_@#$%&*+=/:;,!?()",
    };
    const char *PAGE_NEXT_LABEL[] = {"ABC", "123", "abc"};
    const int PAGE_COUNT = 3;

    const int MAX_KEYS = 40;

    // How long a just-typed password character stays readable before it's
    // masked -- long enough to glance at the hub after the click, short
    // enough that it isn't sitting there for someone else to read.
    const uint32_t PEEK_MS = 3000;

    // Rim geometry on the 240px round panel (visible radius ~120). Keys sit
    // as close to the glass as an 18px glyph allows, and the hub grows out
    // to just inside them, so the whole face is keyboard rather than a
    // ring floating in empty margin.
    //
    // The selected key is centred on the same radius as the rest, NOT
    // pulled inward to make room for its bigger glyph: centring the 32px
    // label box there already puts the middle of a lowercase letter within
    // a pixel of where it sat at 18px (the bigger font's box carries more
    // descender space), and capitals still clear the glass. Pulling it in
    // made the selection visibly drop as it arrived at the top.
    const lv_coord_t RADIUS = 100;
    const lv_coord_t HUB_SIZE = 156;
    const lv_coord_t TEXT_WIDTH = HUB_SIZE - 28; // the hub is round -- leave the text clear of its curve

    // Ring motion, matched to RadialRing (the home dial, Settings, Jobs) so
    // turning the knob feels the same everywhere: the selected key sits at
    // the top and the ring springs round underneath it.
    const uint32_t SPIN_MS = 320;
    // Pushes keys near the top apart, and bunches the far side up, to give
    // the enlarged selected glyph room -- the same curve as
    // RadialRing::setSpread(), kept gentler because a far key still has to
    // be readable enough to tap. The far keys also shrink (see
    // fontForKey()), which is what leaves room to bunch them this much.
    const float SPREAD = 0.35f;
    // Keys fade with distance from the top slot, like the menu chips.
    const lv_opa_t OPA_FAR = 110;

    lv_obj_t *overlay = nullptr;
    lv_obj_t *hub = nullptr;
    lv_obj_t *titleLbl = nullptr;
    lv_obj_t *textLbl = nullptr;
    lv_obj_t *previewLbl = nullptr;
    lv_obj_t *keyLbls[MAX_KEYS] = {nullptr};
    // The font each key label currently has, so layoutKeys() only touches
    // a label's font when it crosses a size band -- a font change forces
    // the label to re-measure, which is too costly for every key on every
    // animation frame.
    const lv_font_t *keyFonts[MAX_KEYS] = {nullptr};

    Key keys[MAX_KEYS];
    int keyCount = 0;
    int selected = 0;
    int prevSelected = -1;
    int page = 0;

    // Each key's centre angle on the unrotated ring (0 = top, clockwise).
    // Keys get a share of the circle in proportion to keyWeight(), not an
    // even slice: an icon is twice a letter's width and "ABC" three times,
    // and even slices sized for letters made them overlap their neighbours.
    float keyCentreDeg[MAX_KEYS] = {0.0f};

    // Ring rotation, in hundredths of a degree. ringPos is the selection's
    // UNWRAPPED position (it keeps counting past keyCount), so the target
    // is always exactly a key's centre plus whole turns however fast the
    // knob spins, and the ring never takes the long way round when it
    // wraps. Same reasoning as RadialRing::targetOffsetX100_.
    int32_t ringPos = 0;
    int32_t ringOffsetX100 = 0;

    char buffer[80];
    size_t bufMax = sizeof(buffer) - 1;
    bool isPassword = false;
    // Password fields only: show everything typed (the rim's eye key), and
    // whether the last character is still in its PEEK_MS window.
    bool revealed = false;
    bool peekLast = false;
    lv_timer_t *peekTimer = nullptr;
    char titleText[24];

    void (*acceptCb)(const char *) = nullptr;
    void (*cancelCb)() = nullptr;

    void refreshText()
    {
        size_t n = strlen(buffer);
        if (n == 0)
        {
            lv_label_set_text(textLbl, "(empty)");
            return;
        }

        // Masked passwords still show their length so it's obvious typing
        // is landing -- a blank field with no feedback is worse than none.
        // The last character stays readable for PEEK_MS after it's typed.
        char shown[sizeof(buffer)];
        for (size_t i = 0; i < n; i++)
        {
            bool visible = !isPassword || revealed || (peekLast && i == n - 1);
            shown[i] = visible ? buffer[i] : '*';
        }
        shown[n] = '\0';

        // Too long for the hub? Show the TAIL, not the head: the end is
        // where typing is happening, and LV_LABEL_LONG_DOT alone would hide
        // exactly the character you're trying to check. Measured rather
        // than a fixed count, since "W" is twice the width of "i".
        const lv_font_t *font = lv_obj_get_style_text_font(textLbl, 0);
        // The label's set width, not lv_obj_get_width(): open() calls this
        // before LVGL has laid the label out, when its measured width is 0.
        const lv_coord_t maxW = TEXT_WIDTH;
        const char *start = shown;
        char tail[sizeof(buffer) + 4];
        snprintf(tail, sizeof(tail), "%s", start);
        while (lv_txt_get_width(tail, strlen(tail), font, 0, LV_TEXT_FLAG_NONE) > maxW && start[1])
        {
            start++;
            snprintf(tail, sizeof(tail), "...%s", start);
        }
        lv_label_set_text(textLbl, tail);
    }

    void peekTimerCb(lv_timer_t *t)
    {
        lv_timer_pause(t);
        peekLast = false;
        if (textLbl) refreshText();
    }

    void startPeek()
    {
        if (!isPassword || !peekTimer) return;
        peekLast = true;
        lv_timer_reset(peekTimer);
        lv_timer_resume(peekTimer);
    }

    void stopPeek()
    {
        peekLast = false;
        if (peekTimer) lv_timer_pause(peekTimer);
    }

    void refreshPreview()
    {
        const Key &k = keys[selected];
        char buf[24];
        switch (k.action)
        {
            case Action::Space:     snprintf(buf, sizeof(buf), "space"); break;
            case Action::Backspace: snprintf(buf, sizeof(buf), "delete"); break;
            case Action::NextPage:  snprintf(buf, sizeof(buf), "%s", PAGE_NEXT_LABEL[page]); break;
            case Action::Reveal:    snprintf(buf, sizeof(buf), revealed ? "hide" : "show"); break;
            case Action::Accept:    snprintf(buf, sizeof(buf), "save"); break;
            case Action::Cancel:    snprintf(buf, sizeof(buf), "cancel"); break;
            case Action::Char:
            default:                snprintf(buf, sizeof(buf), "%c", k.ch); break;
        }
        lv_label_set_text(previewLbl, buf);
    }

    // Keys shrink in bands with distance from the top slot, so the far side
    // -- where the spread bunches them together -- doesn't turn into a
    // solid band of touching glyphs. `nearness` is 1 at the top, 0 opposite.
    // "ABC"/"SP" are words, not glyphs; the rest of the action keys are
    // LVGL symbols, which are much wider than a letter at the same size.
    bool isWordKey(int i) { return keys[i].action == Action::NextPage || keys[i].action == Action::Space; }
    bool isIconKey(int i) { return keys[i].action != Action::Char && !isWordKey(i); }

    // Share of the ring each key takes, relative to a letter.
    float keyWeight(int i)
    {
        if (isWordKey(i)) return 1.6f;
        if (isIconKey(i)) return 1.3f;
        return 1.0f;
    }

    const lv_font_t *fontForKey(int i, float nearness)
    {
        // Icons and words run a size or so behind letters in every band --
        // even with their wider share of the ring, a 32px backspace is as
        // wide as three letters.
        bool sel = (i == selected);
        if (isWordKey(i))
        {
            if (sel) return &lv_font_montserrat_24;
            return nearness > 0.6f ? &lv_font_montserrat_14 : &lv_font_montserrat_12;
        }
        if (isIconKey(i))
        {
            if (sel) return &lv_font_montserrat_24;
            if (nearness > 0.6f) return &lv_font_montserrat_16;
            return nearness > 0.3f ? &lv_font_montserrat_14 : &lv_font_montserrat_12;
        }
        if (sel) return &lv_font_montserrat_32;
        if (nearness > 0.6f) return &lv_font_montserrat_18;
        return nearness > 0.3f ? &lv_font_montserrat_14 : &lv_font_montserrat_12;
    }

    void styleKey(int i, bool sel)
    {
        // Colour only -- size is set by layoutKeys(), from position.
        if (i < 0 || i >= keyCount || !keyLbls[i]) return;
        lv_obj_set_style_text_color(keyLbls[i], sel ? Palette::accent() : Palette::textMuted(), 0);
        if (sel) lv_obj_move_foreground(keyLbls[i]); // over its bunched-up neighbours
    }

    void computeKeyAngles()
    {
        float total = 0.0f;
        for (int i = 0; i < keyCount; i++) total += keyWeight(i);
        float acc = 0.0f;
        for (int i = 0; i < keyCount; i++)
        {
            // Key 0's centre sits at the top, so the ring starts half a
            // slot before it.
            keyCentreDeg[i] = 360.0f * (acc - keyWeight(0) / 2.0f + keyWeight(i) / 2.0f) / total;
            acc += keyWeight(i);
        }
    }

    // The rotation that puts the selected key at the top, including the
    // whole turns ringPos has wound past.
    int32_t ringTargetX100()
    {
        int32_t turns = (ringPos - selected) / keyCount; // exact: selected == ringPos mod keyCount
        return -(int32_t)((keyCentreDeg[selected] + 360.0f * turns) * 100);
    }

    // Positions, fades and sizes every key for the current rotation. Runs
    // on every animation frame; fonts only change as a key crosses a band.
    void layoutKeys()
    {
        float offsetDeg = ringOffsetX100 / 100.0f;
        for (int i = 0; i < keyCount; i++)
        {
            if (!keyLbls[i]) continue;
            float angle = keyCentreDeg[i] + offsetDeg;
            while (angle > 180.0f) angle -= 360.0f;
            while (angle < -180.0f) angle += 360.0f;

            // RadialRing::spreadAngle() with a limit of 180 degrees.
            float u = angle / 180.0f;
            angle = 180.0f * (u + SPREAD * sinf((float)M_PI * u) / (float)M_PI);

            float nearness = 1.0f - fabsf(angle) / 180.0f;
            const lv_font_t *font = fontForKey(i, nearness);
            if (font != keyFonts[i])
            {
                lv_obj_set_style_text_font(keyLbls[i], font, 0);
                keyFonts[i] = font;
            }
            float rad = angle * (float)M_PI / 180.0f;
            lv_obj_align(keyLbls[i], LV_ALIGN_CENTER,
                         (lv_coord_t)(RADIUS * sinf(rad)),
                         (lv_coord_t)(-RADIUS * cosf(rad)));
            lv_obj_set_style_text_opa(keyLbls[i], (lv_opa_t)(OPA_FAR + (LV_OPA_COVER - OPA_FAR) * nearness), 0);
        }
    }

    void ringAnimCb(void *var, int32_t v)
    {
        (void)var;
        ringOffsetX100 = v;
        layoutKeys();
    }

    void spinToRingPos()
    {
        int32_t target = ringTargetX100();
        lv_anim_del(&ringOffsetX100, ringAnimCb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &ringOffsetX100);
        lv_anim_set_exec_cb(&a, ringAnimCb);
        lv_anim_set_values(&a, ringOffsetX100, target);
        lv_anim_set_time(&a, SPIN_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);
    }

    // Moves the selection by `steps` keys (either sign) and spins the ring
    // to bring it to the top.
    void stepSelection(int32_t steps)
    {
        if (keyCount == 0 || steps == 0) return;
        ringPos += steps;
        selected = (int)((ringPos % keyCount + keyCount) % keyCount);
        spinToRingPos();
    }

    void applySelection()
    {
        // Only the two keys that changed state are recoloured; their sizes
        // follow on the next layoutKeys() pass, which the spin runs.
        if (prevSelected != selected)
        {
            styleKey(prevSelected, false);
            styleKey(selected, true);
            prevSelected = selected;
        }
        refreshPreview();
    }

    void keyTapCb(lv_event_t *e)
    {
        // Tapping a rim key only SELECTS it -- spins it to the top -- and the
        // hub commits it. At ~20px apart a mis-tap is likely, and a mis-tap
        // that merely moves the selection costs nothing, whereas one that
        // typed a wrong character would. (RadialRing opens on tap, which is
        // why the keyboard doesn't use it.)
        int target = (int)(intptr_t)lv_event_get_user_data(e);
        int fwd = ((target - selected) % keyCount + keyCount) % keyCount;
        int back = keyCount - fwd;
        stepSelection(fwd <= back ? fwd : -back); // the shorter way round
        applySelection();
    }

    void buildRing()
    {
        // Remember which action key was highlighted (the page switch, or
        // the eye), so the rebuild can land on that same key. The pages
        // hold different numbers of characters, so the action keys sit at
        // a different index on each -- keeping the bare index made the
        // highlight jump to an unrelated key on every page switch.
        bool keepAction = keyCount > 0 && selected < keyCount && keys[selected].action != Action::Char;
        Action heldAction = keepAction ? keys[selected].action : Action::Char;

        for (int i = 0; i < keyCount; i++)
        {
            if (keyLbls[i]) lv_obj_del(keyLbls[i]);
            keyLbls[i] = nullptr;
        }

        keyCount = 0;
        const char *chars = PAGE_CHARS[page];
        for (const char *c = chars; *c && keyCount < MAX_KEYS - 6; c++)
        {
            keys[keyCount].label = nullptr;
            keys[keyCount].ch = *c;
            keys[keyCount].action = Action::Char;
            keyCount++;
        }
        keys[keyCount++] = {PAGE_NEXT_LABEL[page], 0, Action::NextPage};
        keys[keyCount++] = {"SP", ' ', Action::Space};
        keys[keyCount++] = {LV_SYMBOL_BACKSPACE, 0, Action::Backspace};
        if (isPassword) keys[keyCount++] = {revealed ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN, 0, Action::Reveal};
        keys[keyCount++] = {LV_SYMBOL_OK, 0, Action::Accept};
        // The knob long-press cancels too, but nothing on screen said so --
        // touch-only, the editor had no way out except saving.
        keys[keyCount++] = {LV_SYMBOL_CLOSE, 0, Action::Cancel};

        for (int i = 0; i < keyCount; i++)
        {
            lv_obj_t *lbl = lv_label_create(overlay);
            char txt[8];
            if (keys[i].action == Action::Char) snprintf(txt, sizeof(txt), "%c", keys[i].ch);
            else snprintf(txt, sizeof(txt), "%s", keys[i].label);
            lv_label_set_text(lbl, txt);

            lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(lbl, 8);
            lv_obj_add_event_cb(lbl, keyTapCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            keyLbls[i] = lbl;
            keyFonts[i] = nullptr;
            styleKey(i, false);
        }

        if (keepAction)
        {
            for (int i = 0; i < keyCount; i++)
            {
                if (keys[i].action == heldAction)
                {
                    selected = i;
                    break;
                }
            }
        }
        if (selected >= keyCount) selected = 0;

        // Every page has its own key angles, so snap straight to the new
        // ring rather than animating from the old page's geometry.
        lv_anim_del(&ringOffsetX100, ringAnimCb);
        computeKeyAngles();
        ringPos = selected;
        ringOffsetX100 = ringTargetX100();
        layoutKeys();

        prevSelected = -1;
        styleKey(selected, true);
        prevSelected = selected;
        refreshPreview();
        lv_obj_move_foreground(hub); // keys are created after the hub, so re-raise it
    }

    void closeOverlay()
    {
        if (peekTimer)
        {
            lv_timer_del(peekTimer);
            peekTimer = nullptr;
        }
        peekLast = false;
        revealed = false;
        // Don't leave a typed password sitting in a static buffer after the
        // editor has handed it off.
        memset(buffer, 0, sizeof(buffer));
        lv_anim_del(&ringOffsetX100, ringAnimCb); // it positions labels about to be deleted
        if (!overlay) return;
        lv_obj_del(overlay);
        overlay = nullptr;
        hub = titleLbl = textLbl = previewLbl = nullptr;
        for (int i = 0; i < MAX_KEYS; i++) keyLbls[i] = nullptr;
        keyCount = 0;
    }

    void cancelAndClose()
    {
        void (*cb)() = cancelCb;
        acceptCb = nullptr;
        cancelCb = nullptr;
        closeOverlay();
        if (cb) cb();
    }

    void activateSelected()
    {
        const Key &k = keys[selected];
        switch (k.action)
        {
            case Action::Char:
            case Action::Space:
            {
                size_t n = strlen(buffer);
                if (n < bufMax)
                {
                    buffer[n] = (k.action == Action::Space) ? ' ' : k.ch;
                    buffer[n + 1] = '\0';
                    startPeek();
                    refreshText();
                }
                break;
            }
            case Action::Backspace:
            {
                size_t n = strlen(buffer);
                if (n > 0)
                {
                    buffer[n - 1] = '\0';
                    // The peeked character is the one just deleted --
                    // don't let the peek jump to the one before it.
                    stopPeek();
                    refreshText();
                }
                break;
            }
            case Action::NextPage:
                page = (page + 1) % PAGE_COUNT;
                buildRing();
                break;
            case Action::Reveal:
            {
                revealed = !revealed;
                // Rebuild to swap the key's eye glyph; buildRing() keeps
                // the highlight on it, so toggling back is one click.
                buildRing();
                refreshText();
                break;
            }
            case Action::Accept:
            {
                void (*cb)(const char *) = acceptCb;
                acceptCb = nullptr;
                cancelCb = nullptr;
                char finished[sizeof(buffer)];
                strncpy(finished, buffer, sizeof(finished) - 1);
                finished[sizeof(finished) - 1] = '\0';
                closeOverlay();
                if (cb) cb(finished);
                break;
            }
            case Action::Cancel:
                cancelAndClose();
                break;
        }
    }

    void hubTapCb(lv_event_t *e)
    {
        (void)e;
        activateSelected();
    }
}

namespace RadialKeyboard
{
    void open(const char *title, const char *initial, size_t maxLen, bool password,
              void (*onAccept)(const char *), void (*onCancel)())
    {
        closeOverlay();

        acceptCb = onAccept;
        cancelCb = onCancel;
        isPassword = password;
        revealed = false;
        peekLast = false;
        bufMax = (maxLen && maxLen < sizeof(buffer)) ? maxLen : sizeof(buffer) - 1;
        // A password field always starts blank, whatever `initial` holds.
        // Pre-filling it with the saved password would let anyone who can
        // reach the panel open the editor and reveal it; this way the eye
        // key only ever shows what was typed in this session.
        strncpy(buffer, (initial && !password) ? initial : "", sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        if (password)
        {
            peekTimer = lv_timer_create(peekTimerCb, PEEK_MS, nullptr);
            lv_timer_pause(peekTimer);
        }
        strncpy(titleText, title ? title : "", sizeof(titleText) - 1);
        titleText[sizeof(titleText) - 1] = '\0';
        page = 0;
        selected = 0;
        prevSelected = -1;

        overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(overlay, 240, 240);
        lv_obj_center(overlay);
        lv_obj_set_style_bg_color(overlay, Palette::bgApp(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_set_style_radius(overlay, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

        // Centre hub: shows what you're editing and commits the highlighted
        // key. Same styling as the home dial's hub so the gesture reads the
        // same way -- big central target, always in the same place.
        hub = lv_obj_create(overlay);
        lv_obj_set_size(hub, HUB_SIZE, HUB_SIZE);
        lv_obj_center(hub);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(hub, Palette::bgSecondary(), 0);
        lv_obj_set_style_bg_color(hub, Palette::accent(), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(hub, Palette::border(), 0);
        lv_obj_set_style_border_width(hub, 1, 0);
        lv_obj_set_style_pad_all(hub, 0, 0);
        lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hub, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hub, hubTapCb, LV_EVENT_CLICKED, NULL);

        titleLbl = lv_label_create(hub);
        lv_label_set_text(titleLbl, titleText);
        lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLbl, Palette::textMuted(), 0);
        lv_obj_align(titleLbl, LV_ALIGN_CENTER, 0, -42);

        textLbl = lv_label_create(hub);
        lv_obj_set_width(textLbl, TEXT_WIDTH);
        lv_label_set_long_mode(textLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(textLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(textLbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(textLbl, Palette::text(), 0);
        lv_obj_align(textLbl, LV_ALIGN_CENTER, 0, -10);

        // Restates the highlighted key in the middle, so you never have to
        // read the small rim glyph to know what a click will type.
        previewLbl = lv_label_create(hub);
        lv_obj_set_style_text_font(previewLbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(previewLbl, Palette::accent(), 0);
        lv_obj_align(previewLbl, LV_ALIGN_CENTER, 0, 30);

        refreshText();
        buildRing();
    }

    bool isOpen() { return overlay != nullptr; }

    void handleRotate(int32_t delta)
    {
        if (!overlay || keyCount == 0 || delta == 0) return;
        stepSelection(delta);
        applySelection();
    }

    void handleClick()
    {
        if (!overlay) return;
        activateSelected();
    }

    void handleLongPress()
    {
        if (!overlay) return;
        cancelAndClose();
    }
}
