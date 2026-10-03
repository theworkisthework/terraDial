#include "ui_settings.h"
#include "lucide_icons.h"
#include "../config/settings.h"
#include "jog_config.h" // PEN_UP_CMD / PEN_DOWN_CMD, for Reset to defaults
#include "../net/wifi_manager.h"
#include "../net/fluidnc_client.h"
#include "../net/terrapixel_client.h"
#include "radial_ring.h"
#include "ui_nav.h"
#include "palette.h"
#include "ui_widgets.h"
#include "lgfx_config.h" // backlightSet()
#include "radial_keyboard.h"
#include "icon_logo.h"
#include "branding.h"
#include "version.h"
#include "../net/ota_updater.h"
#include "../net/demo_mode.h"
#include "../led/panel_ring.h"
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

// Settings: a radial ring of four categories (Wi-Fi, Machine, Display,
// About), matching Home and Jobs. Selecting one swaps the ring out for that
// category's controls; back returns to the ring.
//
// It was a carousel of four permanently-visible cards -- the last screen
// still using a different browsing idiom, and a card big enough to hold
// several controls leaves nothing to peek at, so the carousel earned
// nothing over a ring. Splitting into "pick a category, then see its
// controls" also gives each control the full width of the panel.
//
// The four panels are built once and kept hidden rather than created on
// demand: the label pointers below (wifiStatusLbl, aboutIpLbl, ...) are
// refreshed every loop by uiSettingsUpdate(), and destroying the panels
// would leave those dangling.
namespace
{
    lv_obj_t *screenRoot = nullptr;
    // Ring geometry, spread toward the top slot like the home dial and Jobs
    // (RadialRing::setSpread) so the selected category is unmistakably the
    // biggest chip rather than one of four near-identical ones.
    //
    // 64 at radius 76 leaves the selected chip 3px clear of the 82px hub,
    // which is what caps the near size here.
    const lv_coord_t RING_RADIUS = 76;
    const lv_coord_t RING_SIZE_NEAR = 64;
    const lv_coord_t RING_SIZE_FAR = 26;
    const float RING_SPREAD = 0.55f;

    RadialRing ring;

    lv_obj_t *hub = nullptr;
    lv_obj_t *hubNameLbl = nullptr;
    lv_obj_t *hubHintLbl = nullptr;

    const int CATEGORY_COUNT = 4;
    lv_obj_t *panels[CATEGORY_COUNT] = {nullptr};
    int openPanel = -1; // -1 = showing the ring

    const char *CATEGORY_NAMES[CATEGORY_COUNT] = {"Wi-Fi", "Machine", "Display", "About"};
    const char *CATEGORY_ICONS[CATEGORY_COUNT] = {
        LUCIDE_WIFI, LUCIDE_SLIDERS_HORIZONTAL, LUCIDE_MONITOR, LUCIDE_INFO};

    // Which icon size each chip is drawn at -- see uiRingIconSize.
    UiRingIconSize iconSize[CATEGORY_COUNT] = {UiRingIconSmall};

    // -- shared text entry --
    // Delegates to the radial keyboard (display/radial_keyboard.h) rather
    // than owning an lv_keyboard overlay: on a 240px round panel a QWERTY
    // map's keys are narrower than a fingertip, so text entry is knob-first
    // here.
    char *editTarget = nullptr;
    size_t editTargetSize = 0;
    void (*editOnSaved)() = nullptr;
    // Fires once the editor closes, on EITHER exit path (saved or
    // canceled) -- unlike onSaved. Used by the Wi-Fi password step to
    // always attempt a reconnect on the way out, so canceling never just
    // leaves the radio silently disconnected (see runScan()'s comment).
    void (*editOnClosed)() = nullptr;

    void fireOnClosed()
    {
        if (!editOnClosed) return;
        void (*cb)() = editOnClosed;
        editOnClosed = nullptr;
        cb();
    }

    void editAccepted(const char *text)
    {
        if (editTarget && editTargetSize > 0)
        {
            strncpy(editTarget, text, editTargetSize - 1);
            editTarget[editTargetSize - 1] = '\0';
            Config::save();
            if (editOnSaved) editOnSaved();
        }
        fireOnClosed();
    }

    void editCancelled() { fireOnClosed(); }

    void openEditor(char *target, size_t targetSize, void (*onSaved)(), bool isPassword,
                    void (*onClosed)() = nullptr, const char *title = "Edit")
    {
        editTarget = target;
        editTargetSize = targetSize;
        editOnSaved = onSaved;
        editOnClosed = onClosed;
        RadialKeyboard::open(title, target, targetSize - 1, isPassword, editAccepted, editCancelled);
    }

    // A category's control panel -- the shared full-face page (see
    // ui_widgets.h), hidden until its category is opened.
    lv_obj_t *makeCardShell(const char *title)
    {
        lv_obj_t *card = uiMakePanel(screenRoot, title);
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
        return card;
    }

    // A heading partway down a card, in the same style as the card's own
    // title, splitting it into sections.
    void makeSectionHeader(lv_obj_t *card, const char *text)
    {
        lv_obj_t *hdr = lv_label_create(card);
        lv_label_set_text(hdr, text);
        // Extra space above, on top of the panel's 8px row gap, so each
        // section reads as its own group. Padding rather than a margin --
        // LVGL 8 has no margin styles.
        lv_obj_set_style_pad_top(hdr, 10, 0);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hdr, Palette::accentSecondary(), 0);
    }

    // ---- Wi-Fi card ----
    lv_obj_t *ssidLbl = nullptr;
    lv_obj_t *passLbl = nullptr;
    lv_obj_t *wifiStatusLbl = nullptr;
    lv_obj_t *forgetBtn = nullptr;
    lv_obj_t *forgetLbl = nullptr;

    void refreshSsidLabel()
    {
        // Tapping this line opens the network scan, so say so -- an
        // unconfigured panel (empty SSID) otherwise shows a bare "SSID: "
        // that reads as static text, leaving the password line as the
        // only obvious control.
        const char *ssid = Config::get().wifiSsid;
        char buf[48];
        if (ssid[0] == '\0')
            snprintf(buf, sizeof(buf), "SSID: (tap to choose)");
        else
            snprintf(buf, sizeof(buf), "SSID: %s " LUCIDE_CHEVRON_RIGHT, ssid);
        lv_label_set_text(ssidLbl, buf);

        // Nothing to forget without a network.
        if (forgetBtn)
        {
            if (ssid[0] == '\0') lv_obj_add_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- Network picker: scan and pick instead of typing an SSID ----
    // secrets.h's WIFI_SSID/WIFI_PASS stay the compile-time defaults
    // (seeded into NVS on first boot, see config/settings.cpp) -- this
    // only changes how a *different* network gets entered on-device.
    lv_obj_t *scanOverlay = nullptr;
    lv_obj_t *scanList = nullptr;
    lv_obj_t *scanStatusLbl = nullptr;
    lv_obj_t *scanRescanBtn = nullptr;

    // Knob highlight: 0..n-1 are the listed networks, n is Rescan -- so
    // an empty or failed scan still leaves the knob something to click.
    int scanSel = 0;

    int scanItemCount() { return (int)lv_obj_get_child_cnt(scanList) + 1; }

    void styleScanSelection()
    {
        int n = (int)lv_obj_get_child_cnt(scanList);
        for (int i = 0; i < n; i++)
        {
            lv_obj_t *btn = lv_obj_get_child(scanList, i);
            bool sel = (i == scanSel);
            lv_obj_set_style_bg_color(btn, sel ? Palette::accent() : Palette::bgPanel(), 0);
            lv_obj_set_style_text_color(btn, sel ? Palette::accentFg() : lv_color_white(), 0);
            if (sel) lv_obj_scroll_to_view(btn, LV_ANIM_ON);
        }
        lv_obj_set_style_bg_color(scanRescanBtn, scanSel == n ? Palette::accent() : Palette::bgSecondary(), 0);
        lv_obj_set_style_text_color(scanRescanBtn, scanSel == n ? Palette::accentFg() : lv_color_white(), 0);
    }

    void closeScanOverlay()
    {
        if (scanOverlay)
        {
            lv_obj_del(scanOverlay);
            scanOverlay = nullptr;
            scanList = nullptr;
            scanStatusLbl = nullptr;
            scanRescanBtn = nullptr;
        }
    }

    // Reconnects using whatever's currently in Config -- called on every
    // way out of the scan+password flow (Cancel at either step, or a
    // completed password entry) so the radio never ends up silently
    // parked disconnected just because scanning required disconnecting
    // first (see runScan()). If nothing was actually picked, this just
    // restores the network that was already configured.
    void reconnectAfterWifiEdit()
    {
        WifiManager::reconnect(Config::get().wifiSsid, Config::get().wifiPass);
    }

    // The network picked from the scan, held back until its password is
    // saved. Committing the SSID on the pick meant canceling the password
    // step still switched networks -- and on a first boot left an SSID
    // saved with no password, so the auto-open setup never came back.
    char pendingSsid[sizeof(AppSettings::wifiSsid)] = "";

    void commitPendingSsid()
    {
        // editAccepted() has already stored the password and saved; this
        // pairs it with the network it was typed for.
        strncpy(Config::get().wifiSsid, pendingSsid, sizeof(Config::get().wifiSsid) - 1);
        Config::get().wifiSsid[sizeof(Config::get().wifiSsid) - 1] = '\0';
        Config::save();
        refreshSsidLabel();
    }

    void pickNetwork(lv_obj_t *btn)
    {
        strncpy(pendingSsid, lv_list_get_btn_text(scanList, btn), sizeof(pendingSsid) - 1);
        pendingSsid[sizeof(pendingSsid) - 1] = '\0';
        closeScanOverlay();
        // Naturally flows into typing the password for the network just
        // picked. Either way out reconnects: onto the new network if saved,
        // back onto the old one if canceled.
        openEditor(Config::get().wifiPass, sizeof(Config::get().wifiPass), commitPendingSsid, true, reconnectAfterWifiEdit, "Password");
    }

    void networkPickedCb(lv_event_t *e) { pickNetwork(lv_event_get_target(e)); }

    void runScan()
    {
        lv_obj_clean(scanList);
        scanSel = 0;
        styleScanSelection();
        lv_label_set_text(scanStatusLbl, "Scanning...");
        // Force the redraw NOW: WiFi.scanNetworks() below blocks this task
        // for seconds, so without this the "Scanning..." label wouldn't
        // reach the panel until the scan had already finished -- the screen
        // just appeared frozen on its previous contents.
        lv_refr_now(NULL);
        // Scanning fails immediately (not after a timeout) if the radio is
        // mid-connect/retry -- confirmed against the Arduino core's
        // WiFiScan.cpp: esp_wifi_scan_start() needs an idle STA. Stop any
        // in-progress connection attempt first so the scan can actually
        // run; every exit from this flow reconnects afterward (see
        // reconnectAfterWifiEdit/scanCancelCb), so this never leaves the
        // device stranded off-network.
        WiFi.disconnect(false, false);
        delay(100);
        int n = WiFi.scanNetworks();
        Serial.printf("[settings] WiFi.scanNetworks() -> %d\n", n);
        if (n == WIFI_SCAN_FAILED)
        {
            lv_label_set_text(scanStatusLbl, "Scan failed -- try Rescan");
            styleScanSelection();
            return;
        }
        if (n == 0)
        {
            lv_label_set_text(scanStatusLbl, "No networks found nearby");
            styleScanSelection();
            return;
        }
        lv_label_set_text(scanStatusLbl, "");
        for (int i = 0; i < n; i++)
        {
            lv_obj_t *btn = lv_list_add_btn(scanList, LUCIDE_WIFI, WiFi.SSID(i).c_str());
            lv_obj_set_style_text_font(btn, &lucide_16, 0); // icon + SSID, inherited by both labels
            lv_obj_add_event_cb(btn, networkPickedCb, LV_EVENT_CLICKED, NULL);
        }
        WiFi.scanDelete();
        styleScanSelection();
    }

    void rescanCb(lv_event_t *e) { (void)e; runScan(); }

    void scanCancelCb(lv_event_t *e)
    {
        (void)e;
        closeScanOverlay();
        reconnectAfterWifiEdit();
    }

    void openScanOverlay()
    {
        scanOverlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(scanOverlay, 240, 240);
        lv_obj_set_style_bg_color(scanOverlay, Palette::bgApp(), 0);
        lv_obj_set_style_bg_opa(scanOverlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(scanOverlay, 0, 0);
        lv_obj_clear_flag(scanOverlay, LV_OBJ_FLAG_SCROLLABLE);

        // Deliberate, individually-placed positions rather than a flex
        // layout: this is a round screen, and flex's top-packed stacking
        // pushed content into the corners/edges the physical glass doesn't
        // cover (same root cause as the cancel-button fix in openEditor()
        // above -- flex layout also silently overrides any manual
        // lv_obj_align() on its own children, so the two don't mix here).
        lv_obj_t *cancelBtn = lv_btn_create(scanOverlay);
        lv_obj_set_size(cancelBtn, 28, 28);
        lv_obj_set_style_radius(cancelBtn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cancelBtn, Palette::bgSecondary(), 0);
        lv_obj_align(cancelBtn, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_add_event_cb(cancelBtn, scanCancelCb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cancelLbl = lv_label_create(cancelBtn);
        lv_label_set_text(cancelLbl, LUCIDE_X);
        lv_obj_set_style_text_font(cancelLbl, &lucide_12, 0);
        lv_obj_center(cancelLbl);

        lv_obj_t *titleLbl = lv_label_create(scanOverlay);
        lv_label_set_text(titleLbl, "Select a network");
        lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLbl, lv_color_white(), 0);
        lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 46);

        scanStatusLbl = lv_label_create(scanOverlay);
        lv_obj_set_style_text_font(scanStatusLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(scanStatusLbl, Palette::textMuted(), 0);
        lv_obj_align(scanStatusLbl, LV_ALIGN_TOP_MID, 0, 68);

        scanList = lv_list_create(scanOverlay);
        lv_obj_set_size(scanList, 190, 108);
        lv_obj_align(scanList, LV_ALIGN_CENTER, 0, 8);
        lv_obj_set_style_bg_opa(scanList, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scanList, 0, 0);

        scanRescanBtn = lv_btn_create(scanOverlay);
        lv_obj_set_size(scanRescanBtn, 100, 30);
        lv_obj_set_style_radius(scanRescanBtn, 15, 0);
        lv_obj_align(scanRescanBtn, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_add_event_cb(scanRescanBtn, rescanCb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *rescanLbl = lv_label_create(scanRescanBtn);
        lv_label_set_text(rescanLbl, "Rescan");
        lv_obj_set_style_text_font(rescanLbl, &lv_font_montserrat_12, 0);
        lv_obj_center(rescanLbl);

        runScan();
    }

    void ssidCb(lv_event_t *e)
    {
        (void)e;
        openScanOverlay();
    }

    void passCb(lv_event_t *e)
    {
        (void)e;
        openEditor(Config::get().wifiPass, sizeof(Config::get().wifiPass), nullptr, true, reconnectAfterWifiEdit, "Password");
    }

    // ---- Forget network ----
    // Two taps: the first arms it for FORGET_ARM_MS, the second clears the
    // saved SSID and password. One stray tap on a panel that only reaches
    // the plotter over Wi-Fi shouldn't be able to strand it.
    const uint32_t FORGET_ARM_MS = 3000;
    lv_timer_t *forgetArmTimer = nullptr;

    void disarmForget()
    {
        if (forgetArmTimer)
        {
            lv_timer_del(forgetArmTimer);
            forgetArmTimer = nullptr;
        }
        if (!forgetBtn) return;
        lv_obj_set_style_bg_color(forgetBtn, Palette::bgSecondary(), 0);
        lv_label_set_text(forgetLbl, "Forget network");
    }

    void forgetCb(lv_event_t *e)
    {
        (void)e;
        if (!forgetArmTimer)
        {
            lv_obj_set_style_bg_color(forgetBtn, Palette::alert(), 0);
            lv_label_set_text(forgetLbl, "Tap again to forget");
            forgetArmTimer = lv_timer_create([](lv_timer_t *) { disarmForget(); }, FORGET_ARM_MS, nullptr);
            lv_timer_set_repeat_count(forgetArmTimer, 1);
            return;
        }

        // Stored as empty strings rather than removed from NVS: a removed
        // key would fall back to secrets.h's compile-time default, so a dev
        // build with credentials baked in would quietly rejoin the network
        // you just forgot.
        disarmForget();
        Config::get().wifiSsid[0] = '\0';
        Config::get().wifiPass[0] = '\0';
        Config::save();
        refreshSsidLabel();
        WifiManager::reconnect("", ""); // drops the current connection
        // Straight into picking a new one, same as a first boot. Canceling
        // the scan leaves the panel unconfigured, and the next boot opens
        // it again.
        openScanOverlay();
    }

    void connectCb(lv_event_t *e)
    {
        (void)e;
        lv_label_set_text(wifiStatusLbl, "Connecting...");
        WifiManager::reconnect(Config::get().wifiSsid, Config::get().wifiPass);
    }

    lv_obj_t *makeWifiCard()
    {
        lv_obj_t *card = makeCardShell("WI-FI");

        ssidLbl = lv_label_create(card);
        lv_obj_set_style_text_font(ssidLbl, &lucide_12, 0); // text + a chevron
        lv_obj_add_flag(ssidLbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ssidLbl, ssidCb, LV_EVENT_CLICKED, NULL);
        refreshSsidLabel();

        passLbl = lv_label_create(card);
        lv_label_set_text(passLbl, "Password: (tap to edit)");
        lv_obj_set_style_text_font(passLbl, &lv_font_montserrat_12, 0);
        lv_obj_add_flag(passLbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(passLbl, passCb, LV_EVENT_CLICKED, NULL);

        wifiStatusLbl = lv_label_create(card);
        lv_obj_set_style_text_font(wifiStatusLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(wifiStatusLbl, Palette::textMuted(), 0);
        lv_label_set_text(wifiStatusLbl, "--");

        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, 110, 34);
        lv_obj_set_style_bg_color(btn, Palette::accent(), 0);
        lv_obj_set_style_radius(btn, 17, 0);
        lv_obj_add_event_cb(btn, connectCb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *btnLbl = lv_label_create(btn);
        lv_label_set_text(btnLbl, "Connect");
        lv_obj_set_style_text_color(btnLbl, Palette::accentFg(), 0);
        lv_obj_center(btnLbl);

        forgetBtn = lv_btn_create(card);
        lv_obj_set_size(forgetBtn, 150, 30);
        lv_obj_set_style_radius(forgetBtn, 15, 0);
        lv_obj_add_event_cb(forgetBtn, forgetCb, LV_EVENT_CLICKED, NULL);
        forgetLbl = lv_label_create(forgetBtn);
        lv_obj_set_style_text_font(forgetLbl, &lv_font_montserrat_12, 0);
        lv_obj_center(forgetLbl);
        disarmForget();
        refreshSsidLabel(); // sets the button's visibility

        return card;
    }

    // ---- Machine card (FluidNC/terraPixel hosts, pen commands) ----
    lv_obj_t *fncHostLbl = nullptr;
    lv_obj_t *tpHostLbl = nullptr;
    lv_obj_t *penUpLbl = nullptr;
    lv_obj_t *penDownLbl = nullptr;

    void refreshHostLabels()
    {
        lv_label_set_text(fncHostLbl, Config::get().fluidNcHost);
        lv_label_set_text(tpHostLbl, Config::get().terraPixelHost);
    }

    // Applied immediately: without the hostChanged() nudge the clients kept
    // whatever address they had first resolved until a reboot.
    void fncHostSaved()
    {
        refreshHostLabels();
        fluidNC.hostChanged();
    }

    void tpHostSaved()
    {
        refreshHostLabels();
        terraPixel.hostChanged();
    }

    void fncHostCb(lv_event_t *e)
    {
        (void)e;
        openEditor(Config::get().fluidNcHost, sizeof(Config::get().fluidNcHost), fncHostSaved, false, nullptr, "FluidNC host");
    }

    void tpHostCb(lv_event_t *e)
    {
        (void)e;
        openEditor(Config::get().terraPixelHost, sizeof(Config::get().terraPixelHost), tpHostSaved, false, nullptr, "terraPixel host");
    }

    // ---- pen up/down commands ----
    // The G-code the Pen screen sends for each state -- the same pair
    // terraForge keeps per machine. Swapping exchanges the two strings
    // rather than inverting anything, so a machine whose pen lifts on -Z
    // (or a servo/solenoid driven by M-codes) is set up by what the
    // commands say, and the values are never rewritten behind your back.
    void refreshPenCmdLabels()
    {
        const AppSettings &cfg = Config::get();
        lv_label_set_text(penUpLbl, cfg.penUpCmd[0] ? cfg.penUpCmd : "(none)");
        lv_label_set_text(penDownLbl, cfg.penDownCmd[0] ? cfg.penDownCmd : "(none)");
    }

    void penUpCmdCb(lv_event_t *e)
    {
        (void)e;
        openEditor(Config::get().penUpCmd, sizeof(Config::get().penUpCmd), refreshPenCmdLabels, false, nullptr, "Pen up");
    }

    void penDownCmdCb(lv_event_t *e)
    {
        (void)e;
        openEditor(Config::get().penDownCmd, sizeof(Config::get().penDownCmd), refreshPenCmdLabels, false, nullptr, "Pen down");
    }

    void penSwapCb(lv_event_t *e)
    {
        (void)e;
        AppSettings &cfg = Config::get();
        char tmp[sizeof(cfg.penUpCmd)];
        strcpy(tmp, cfg.penUpCmd);
        strcpy(cfg.penUpCmd, cfg.penDownCmd);
        strcpy(cfg.penDownCmd, tmp);
        Config::save();
        refreshPenCmdLabels();
    }

    // ---- Reset to defaults ----
    // Two taps, like Forget network: terraForge's reset only touches an
    // unsaved form, but here every change saves at once, and a macro typed
    // out on the radial keyboard is too slow to re-enter to lose to a
    // stray tap.
    const uint32_t PEN_RESET_ARM_MS = 3000;
    const char *PEN_RESET_TEXT = LUCIDE_ROTATE_CCW " Reset to defaults";
    lv_obj_t *penResetBtn = nullptr;
    lv_obj_t *penResetLbl = nullptr;
    lv_timer_t *penResetArmTimer = nullptr;

    void disarmPenReset()
    {
        if (penResetArmTimer)
        {
            lv_timer_del(penResetArmTimer);
            penResetArmTimer = nullptr;
        }
        lv_obj_set_style_bg_color(penResetBtn, Palette::bgSecondary(), 0);
        lv_label_set_text(penResetLbl, PEN_RESET_TEXT);
    }

    void penResetCb(lv_event_t *e)
    {
        (void)e;
        if (!penResetArmTimer)
        {
            lv_obj_set_style_bg_color(penResetBtn, Palette::alert(), 0);
            lv_label_set_text(penResetLbl, "Tap again to reset");
            penResetArmTimer = lv_timer_create([](lv_timer_t *) { disarmPenReset(); }, PEN_RESET_ARM_MS, nullptr);
            lv_timer_set_repeat_count(penResetArmTimer, 1);
            return;
        }

        disarmPenReset();
        AppSettings &cfg = Config::get();
        strncpy(cfg.penUpCmd, PEN_UP_CMD, sizeof(cfg.penUpCmd) - 1);
        cfg.penUpCmd[sizeof(cfg.penUpCmd) - 1] = '\0';
        strncpy(cfg.penDownCmd, PEN_DOWN_CMD, sizeof(cfg.penDownCmd) - 1);
        cfg.penDownCmd[sizeof(cfg.penDownCmd) - 1] = '\0';
        Config::save();
        refreshPenCmdLabels();
    }

    // terraForge's "secondary" button: the raised sea-blue surface rather
    // than uiMakeButton's accent fill, with an icon leading the text.
    lv_obj_t *makeSecondaryButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **outLabel = nullptr)
    {
        lv_obj_t *lbl = nullptr;
        lv_obj_t *btn = uiMakeButton(parent, text, &lbl);
        lv_obj_set_style_bg_color(btn, Palette::bgSecondary(), 0);
        lv_obj_set_style_bg_color(btn, Palette::bgSecondaryHover(), LV_STATE_PRESSED);
        lv_obj_set_style_text_font(lbl, &lucide_12, 0); // icon + text
        lv_obj_set_style_text_color(lbl, Palette::text(), 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        if (outLabel) *outLabel = lbl;
        return btn;
    }

    void addPenControls(lv_obj_t *card)
    {
        makeSectionHeader(card, "PEN");

        lv_obj_t *upField = uiMakeTextField(card, "Pen up command", &penUpLbl);
        lv_obj_add_event_cb(upField, penUpCmdCb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *downField = uiMakeTextField(card, "Pen down command", &penDownLbl);
        lv_obj_add_event_cb(downField, penDownCmdCb, LV_EVENT_CLICKED, NULL);
        refreshPenCmdLabels();

        // terraForge's two secondary buttons, under the fields they act
        // on. Stacked rather than side by side as there: half of the 180px
        // column is too narrow for either label.
        makeSecondaryButton(card, LUCIDE_ARROW_UP_DOWN " Swap up / down", penSwapCb);
        penResetBtn = makeSecondaryButton(card, PEN_RESET_TEXT, penResetCb, &penResetLbl);
    }

    // ---- terraPixel: an optional extra, switched on here ----
    // Off by default -- most machines have no terraPixel. While off, the
    // host field and its status are hidden, nothing contacts terraPixel,
    // and the dial drops its Lights item (ui_dial.cpp).
    lv_obj_t *tpDetails = nullptr;
    lv_obj_t *tpStatusLbl = nullptr;

    // Hosts equal apart from case and an mDNS ".local" suffix, which is
    // optional on both fields.
    bool sameHost(const char *a, const char *b)
    {
        size_t la = strlen(a), lb = strlen(b);
        if (la > 6 && !strcasecmp(a + la - 6, ".local")) la -= 6;
        if (lb > 6 && !strcasecmp(b + lb - 6, ".local")) lb -= 6;
        return la && la == lb && !strncasecmp(a, b, la);
    }

    // The likeliest wrong entry is the plotter's own address -- the dial
    // talks to that, so it reads like the obvious answer. Said outright
    // rather than left to show up as "Not a terraPixel".
    void refreshTpStatus()
    {
        if (!tpStatusLbl) return;
        const AppSettings &cfg = Config::get();
        const char *text = "Searching...";
        lv_color_t col = Palette::textMuted();
        if (sameHost(cfg.terraPixelHost, cfg.fluidNcHost))
        {
            text = "That's your plotter's address";
            col = Palette::accent();
        }
        else
        {
            switch (terraPixel.status().link)
            {
                case TerraPixelLink::Connected:
                    text = "Connected";
                    col = lv_color_hex(0x3ddc84); // the dial's Run/Done green
                    break;
                case TerraPixelLink::NotFound:
                    text = "Not found";
                    col = Palette::accent();
                    break;
                case TerraPixelLink::WrongDevice:
                    text = "Not a terraPixel";
                    col = Palette::accent();
                    break;
                default:
                    break;
            }
        }
        if (strcmp(lv_label_get_text(tpStatusLbl), text) != 0) lv_label_set_text(tpStatusLbl, text);
        lv_obj_set_style_text_color(tpStatusLbl, col, 0);
    }

    void tpEnabledCb(lv_event_t *e)
    {
        lv_obj_t *sw = lv_event_get_target(e);
        bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
        Config::get().terraPixelEnabled = on;
        Config::save();
        if (on) lv_obj_clear_flag(tpDetails, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tpDetails, LV_OBJ_FLAG_HIDDEN);
        refreshTpStatus();
    }

    void addTerraPixelControls(lv_obj_t *card)
    {
        makeSectionHeader(card, "TERRAPIXEL");

        lv_obj_t *row = uiMakeRow(card, "Rail lights");
        lv_obj_t *sw = uiMakeSwitch(row, Config::get().terraPixelEnabled);
        lv_obj_add_event_cb(sw, tpEnabledCb, LV_EVENT_VALUE_CHANGED, NULL);

        // Everything that only means something once switched on, in one
        // container so the switch can show or hide it as a unit.
        tpDetails = lv_obj_create(card);
        lv_obj_set_size(tpDetails, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(tpDetails, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tpDetails, 0, 0);
        lv_obj_set_style_pad_all(tpDetails, 0, 0);
        lv_obj_set_style_pad_row(tpDetails, 6, 0);
        lv_obj_clear_flag(tpDetails, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(tpDetails, LV_FLEX_FLOW_COLUMN);
        if (!Config::get().terraPixelEnabled) lv_obj_add_flag(tpDetails, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *tpField = uiMakeTextField(tpDetails, "Host / IP", &tpHostLbl);
        lv_obj_add_event_cb(tpField, tpHostCb, LV_EVENT_CLICKED, NULL);

        tpStatusLbl = lv_label_create(tpDetails);
        lv_label_set_text(tpStatusLbl, "");
        lv_obj_set_style_text_font(tpStatusLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_left(tpStatusLbl, 2, 0); // in line with the field's caption
        refreshTpStatus();

        lv_obj_t *hint = lv_label_create(tpDetails);
        lv_label_set_text(hint, "terraPixel's own address -- not the plotter's.");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_pad_left(hint, 2, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, Palette::textFaint(), 0);
    }

    lv_obj_t *makeMachineCard()
    {
        lv_obj_t *card = makeCardShell("MACHINE");

        // "Host / IP", as terraForge labels its connection field. The card's
        // own MACHINE title heads this first section; the pen follows the
        // plotter it belongs to, and terraPixel is a separate device, so it
        // gets its own section.
        lv_obj_t *fncField = uiMakeTextField(card, "Host / IP", &fncHostLbl);
        lv_obj_add_event_cb(fncField, fncHostCb, LV_EVENT_CLICKED, NULL);

        addPenControls(card);

        addTerraPixelControls(card);
        refreshHostLabels();

        return card;
    }

    // ---- Display card (brightness, menu direction, screen sleep) ----
    lv_obj_t *sleepLedLbl = nullptr;
    lv_obj_t *brightLbl = nullptr;
    lv_obj_t *ringBrightLbl = nullptr;

    void ringBrightSliderCb(lv_event_t *e)
    {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        int v = lv_slider_get_value(slider);
        char buf[32];
        snprintf(buf, sizeof(buf), "Ring brightness: %d%%", v);
        lv_label_set_text(ringBrightLbl, buf);
        // Live, like the backlight -- you're looking at the ring as you drag.
        Config::get().ringBrightnessPct = (uint8_t)v;
        panelRing.setBrightness((uint8_t)v);
        if (lv_event_get_code(e) == LV_EVENT_RELEASED) Config::save();
    }

    void brightSliderCb(lv_event_t *e)
    {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        int v = lv_slider_get_value(slider);
        char buf[24];
        snprintf(buf, sizeof(buf), "Brightness: %d%%", v);
        lv_label_set_text(brightLbl, buf);

        // Apply live while dragging so the slider actually does something
        // visible -- this screen previously only had the auto-dim TIMEOUT
        // slider, which is why "the backlight slider" appeared to do nothing
        // to brightness.
        Config::get().backlightBrightnessPct = (uint8_t)v;
        backlightSet((uint8_t)v);

        if (lv_event_get_code(e) == LV_EVENT_RELEASED) Config::save();
    }

    void idleLogoCb(lv_event_t *e)
    {
        lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
        Config::get().showIdleLogo = lv_obj_has_state(sw, LV_STATE_CHECKED);
        Config::save();
    }

    void invertRotCb(lv_event_t *e)
    {
        lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
        Config::get().invertMenuRotation = lv_obj_has_state(sw, LV_STATE_CHECKED);
        Config::save();
    }

    // Sleep timeout as discrete chips rather than a slider: these are a few
    // named choices, not a continuum, and chips are a far easier touch
    // target than hitting an exact second on a 140px slider.
    const int SLEEP_OPTION_COUNT = 4;
    const uint16_t SLEEP_OPTION_SECS[SLEEP_OPTION_COUNT] = {0, 180, 300, 600};
    const char *SLEEP_OPTION_LABELS[SLEEP_OPTION_COUNT] = {"Never", "3m", "5m", "10m"};
    lv_obj_t *sleepChips[SLEEP_OPTION_COUNT] = {nullptr};
    lv_obj_t *sleepChipLbls[SLEEP_OPTION_COUNT] = {nullptr};

    void restyleSleepChips()
    {
        for (int i = 0; i < SLEEP_OPTION_COUNT; i++)
        {
            bool sel = Config::get().sleepTimeoutSec == SLEEP_OPTION_SECS[i];
            lv_obj_set_style_bg_color(sleepChips[i], sel ? Palette::accent() : Palette::bgPanel(), 0);
            lv_obj_set_style_text_color(sleepChipLbls[i], sel ? Palette::accentFg() : Palette::textMuted(), 0);
        }
    }

    void sleepChipCb(lv_event_t *e)
    {
        int i = (int)(intptr_t)lv_event_get_user_data(e);
        Config::get().sleepTimeoutSec = SLEEP_OPTION_SECS[i];
        Config::save();
        restyleSleepChips();
    }

    void sleepLedSliderCb(lv_event_t *e)
    {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        int v = lv_slider_get_value(slider);
        char buf[40];
        snprintf(buf, sizeof(buf), "Ring sleep brightness: %d%%", v);
        lv_label_set_text(sleepLedLbl, buf);
        Config::get().sleepLedBrightnessPct = (uint8_t)v;
        if (lv_event_get_code(e) == LV_EVENT_RELEASED) Config::save();
    }

    lv_obj_t *makeDisplayCard()
    {
        lv_obj_t *card = makeCardShell("DISPLAY");

        lv_obj_t *brightRow = uiMakeRow(card);
        brightLbl = lv_label_create(brightRow);
        lv_obj_set_style_text_font(brightLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(brightLbl, Palette::textMuted(), 0);
        // Floor of 10%: 0 would black the panel out with no way to see the
        // slider well enough to turn it back up.
        lv_obj_t *brightSlider = uiMakeSlider(brightRow, 10, 100, Config::get().backlightBrightnessPct);
        lv_obj_add_event_cb(brightSlider, brightSliderCb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(brightSlider, brightSliderCb, LV_EVENT_RELEASED, NULL);
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "Brightness: %d%%", Config::get().backlightBrightnessPct);
            lv_label_set_text(brightLbl, buf);
        }

        // The panel's own LED ring, under the screen's brightness. Not
        // terraPixel's rail -- that's on the Lights screen.
        lv_obj_t *ringRow = uiMakeRow(card);
        ringBrightLbl = lv_label_create(ringRow);
        lv_obj_set_style_text_font(ringBrightLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ringBrightLbl, Palette::textMuted(), 0);
        lv_obj_t *ringSlider = uiMakeSlider(ringRow, 0, 100, Config::get().ringBrightnessPct);
        lv_obj_add_event_cb(ringSlider, ringBrightSliderCb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(ringSlider, ringBrightSliderCb, LV_EVENT_RELEASED, NULL);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "Ring brightness: %d%%", Config::get().ringBrightnessPct);
            lv_label_set_text(ringBrightLbl, buf);
        }

        lv_obj_t *logoRow = uiMakeRow(card, "Idle logo");
        lv_obj_t *logoSw = uiMakeSwitch(logoRow, Config::get().showIdleLogo);
        lv_obj_add_event_cb(logoSw, idleLogoCb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *invertRow = uiMakeRow(card, "Invert menu rotation");
        lv_obj_t *invertSw = uiMakeSwitch(invertRow, Config::get().invertMenuRotation);
        lv_obj_add_event_cb(invertSw, invertRotCb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *sleepRow = uiMakeRow(card, "Sleep after");
        lv_obj_t *chipRow = lv_obj_create(sleepRow);
        lv_obj_set_size(chipRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(chipRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chipRow, 0, 0);
        lv_obj_set_style_pad_all(chipRow, 0, 0);
        lv_obj_set_style_pad_column(chipRow, 4, 0);
        lv_obj_clear_flag(chipRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chipRow, LV_FLEX_FLOW_ROW);
        for (int i = 0; i < SLEEP_OPTION_COUNT; i++)
        {
            lv_obj_t *chip = lv_obj_create(chipRow);
            lv_obj_set_size(chip, 38, 26);
            lv_obj_set_style_radius(chip, 13, 0);
            lv_obj_set_style_border_width(chip, 0, 0);
            lv_obj_set_style_pad_all(chip, 0, 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_ext_click_area(chip, 4);
            lv_obj_add_event_cb(chip, sleepChipCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lbl = lv_label_create(chip);
            lv_label_set_text(lbl, SLEEP_OPTION_LABELS[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(lbl);
            sleepChips[i] = chip;
            sleepChipLbls[i] = lbl;
        }
        restyleSleepChips();

        // The ring stays lit while the screen is off, so machine state is
        // still readable across the room mid-plot.
        lv_obj_t *sleepLedRow = uiMakeRow(card);
        sleepLedLbl = lv_label_create(sleepLedRow);
        lv_obj_set_style_text_font(sleepLedLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sleepLedLbl, Palette::textMuted(), 0);
        lv_obj_t *sleepLedSlider = uiMakeSlider(sleepLedRow, 0, 100, Config::get().sleepLedBrightnessPct);
        lv_obj_add_event_cb(sleepLedSlider, sleepLedSliderCb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sleepLedSlider, sleepLedSliderCb, LV_EVENT_RELEASED, NULL);
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "Ring sleep brightness: %d%%", Config::get().sleepLedBrightnessPct);
            lv_label_set_text(sleepLedLbl, buf);
        }

        return card;
    }

    // ---- About card: identity, links, diagnostics ----
    lv_obj_t *aboutIpLbl = nullptr;
    lv_obj_t *aboutUptimeLbl = nullptr;

    // A QR plus its caption. Skipped entirely when the URL is empty, so an
    // unfilled link (see include/branding.h) leaves no dead code on screen.
    void addLinkQr(lv_obj_t *parent, const char *caption, const char *url)
    {
        if (!url || !url[0]) return;

        lv_obj_t *capLbl = lv_label_create(parent);
        lv_label_set_text(capLbl, caption);
        lv_obj_set_style_text_font(capLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(capLbl, Palette::textMuted(), 0);

        // Light background with dark modules, NOT the UI palette inverted:
        // scanners are far more reliable on conventional polarity, and a
        // code nobody's phone will read is decoration, not a link.
        lv_obj_t *qr = lv_qrcode_create(parent, 104, Palette::bgApp(), lv_color_white());
        lv_qrcode_update(qr, url, strlen(url));
        // Quiet zone: the spec wants clear space around the symbol, and
        // without it the dark UI crowds the edge modules.
        lv_obj_set_style_border_color(qr, lv_color_white(), 0);
        lv_obj_set_style_border_width(qr, 4, 0);
    }

    // ---- firmware update, on the About card ----
    // Not a fifth Settings category: the ring's geometry is tuned for four
    // (see the arc comment in uiSettingsCreate), and "what version am I on"
    // is an About question anyway -- the version label and the button that
    // changes it belong on the same card.
    lv_obj_t *updateBtn = nullptr;
    lv_obj_t *updateBtnLbl = nullptr;
    lv_obj_t *updateStatusLbl = nullptr;
    lv_obj_t *updateBar = nullptr;

    void updateBtnCb(lv_event_t *e)
    {
        (void)e;
        // One button, three jobs, decided by where the updater has got to.
        // A separate "install" button would sit dead on screen for the
        // entire life of a panel that is already up to date.
        switch (OtaUpdater::state())
        {
            case OtaUpdater::State::Available:
                OtaUpdater::requestInstall();
                break;
            case OtaUpdater::State::Checking:
            case OtaUpdater::State::Installing:
            case OtaUpdater::State::Done:
                break; // in flight -- the label already says so
            default:
                OtaUpdater::requestCheck();
                break;
        }
    }

    // Mirrors the updater's state onto the three widgets above. Called from
    // uiSettingsUpdate(), i.e. on the UI core, reading values the network
    // task wrote -- see the threading note in net/ota_updater.h.
    void refreshUpdateWidgets()
    {
        if (!updateBtnLbl) return;

        OtaUpdater::State st = OtaUpdater::state();

        const char *btnText = "Check for updates";
        bool enabled = true;
        switch (st)
        {
            case OtaUpdater::State::Checking:   btnText = "Checking..."; enabled = false; break;
            case OtaUpdater::State::Available:  btnText = "Install update"; break;
            case OtaUpdater::State::Installing: btnText = "Installing..."; enabled = false; break;
            case OtaUpdater::State::Done:       btnText = "Restarting..."; enabled = false; break;
            case OtaUpdater::State::UpToDate:
            case OtaUpdater::State::Failed:     btnText = "Check again"; break;
            default: break;
        }

        if (strcmp(lv_label_get_text(updateBtnLbl), btnText) != 0)
            lv_label_set_text(updateBtnLbl, btnText);

        // Greyed rather than hidden: a button that vanishes mid-tap moves
        // everything below it up under the finger. The state goes on the
        // label as well as the button because LVGL doesn't propagate object
        // state to children, and the label carries its own text colour.
        if (enabled)
        {
            lv_obj_clear_state(updateBtn, LV_STATE_DISABLED);
            lv_obj_clear_state(updateBtnLbl, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(updateBtn, LV_STATE_DISABLED);
            lv_obj_add_state(updateBtnLbl, LV_STATE_DISABLED);
        }

        const char *msg = OtaUpdater::message();
        if (strcmp(lv_label_get_text(updateStatusLbl), msg) != 0)
            lv_label_set_text(updateStatusLbl, msg);
        lv_obj_set_style_text_color(
            updateStatusLbl,
            st == OtaUpdater::State::Failed ? Palette::accent() : Palette::textMuted(), 0);

        if (st == OtaUpdater::State::Installing)
        {
            lv_obj_clear_flag(updateBar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(updateBar, OtaUpdater::progressPct(), LV_ANIM_OFF);
        }
        else
        {
            lv_obj_add_flag(updateBar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void addUpdateControls(lv_obj_t *card)
    {
        makeSectionHeader(card, "FIRMWARE");

        updateBtn = uiMakeButton(card, "Check for updates", &updateBtnLbl);
        lv_obj_add_event_cb(updateBtn, updateBtnCb, LV_EVENT_CLICKED, NULL);

        updateStatusLbl = lv_label_create(card);
        // Wrapped and centred: the failure messages ("Machine busy -- try
        // when idle") are longer than the 180px content column.
        lv_obj_set_width(updateStatusLbl, lv_pct(100));
        lv_label_set_long_mode(updateStatusLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(updateStatusLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(updateStatusLbl, "");
        lv_obj_set_style_text_font(updateStatusLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(updateStatusLbl, Palette::textMuted(), 0);

        updateBar = lv_bar_create(card);
        lv_obj_set_size(updateBar, lv_pct(100), 8);
        lv_obj_set_style_bg_color(updateBar, Palette::bgPanel(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(updateBar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(updateBar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(updateBar, Palette::accent(), LV_PART_INDICATOR);
        lv_obj_set_style_radius(updateBar, 4, LV_PART_INDICATOR);
        lv_bar_set_range(updateBar, 0, 100);
        lv_obj_add_flag(updateBar, LV_OBJ_FLAG_HIDDEN);
    }

    void demoModeCb(lv_event_t *e)
    {
        lv_obj_t *sw = lv_event_get_target(e);
        Demo::set(lv_obj_has_state(sw, LV_STATE_CHECKED));
    }

    // Demo mode (net/demo_mode.h): a simulated plotter and lights, so the
    // panel can be shown working with no machine around. Here rather than
    // on a main screen because it's a thing you set up once for a showing,
    // not something to hit by accident mid-job.
    void addDemoControls(lv_obj_t *card)
    {
        lv_obj_t *row = uiMakeRow(card, "Demo mode");
        lv_obj_t *sw = uiMakeSwitch(row, Demo::isOn());
        lv_obj_add_event_cb(sw, demoModeCb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint, "Simulated plotter and lights -- nothing is sent to the machine. Left untouched for 20s it tours itself; touch to take over. Off again after a restart.");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, Palette::textMuted(), 0);
    }

    lv_obj_t *makeAboutCard()
    {
        lv_obj_t *card = makeCardShell("ABOUT");

        lv_obj_t *logo = lv_img_create(card);
        lv_img_set_src(logo, &iconLogo);
        // Alpha-only: recolor_opa must be on or it draws nothing. This is
        // also what flips the print artwork's black stroke to light-on-dark.
        lv_obj_set_style_img_recolor(logo, Palette::text(), 0);
        lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);

        lv_obj_t *nameLbl = lv_label_create(card);
        lv_label_set_text(nameLbl, Branding::productName());
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(nameLbl, Palette::text(), 0);

        lv_obj_t *siteLbl = lv_label_create(card);
        lv_label_set_text(siteLbl, Branding::siteLabel());
        lv_obj_set_style_text_font(siteLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(siteLbl, Palette::accent(), 0);

        // Directly under the name, because the first question anyone asks
        // of an About screen is which build they're looking at -- and it's
        // the first thing worth quoting when reporting a fault.
        lv_obj_t *versionLbl = lv_label_create(card);
        char versionBuf[32];
        snprintf(versionBuf, sizeof(versionBuf), "Firmware %s", Version::firmware());
        lv_label_set_text(versionLbl, versionBuf);
        lv_obj_set_style_text_font(versionLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(versionLbl, Palette::textMuted(), 0);

        addLinkQr(card, "terrapen.xyz", Branding::siteUrl());
        addLinkQr(card, "Source on GitHub", Branding::githubUrl());
        addLinkQr(card, "Discord", Branding::discordUrl());

        lv_obj_t *hostLbl = lv_label_create(card);
        lv_label_set_text(hostLbl, Branding::mdnsAddress());
        lv_obj_set_style_text_font(hostLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hostLbl, Palette::text(), 0);

        aboutIpLbl = lv_label_create(card);
        lv_obj_set_style_text_font(aboutIpLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(aboutIpLbl, Palette::textMuted(), 0);

        aboutUptimeLbl = lv_label_create(card);
        lv_obj_set_style_text_font(aboutUptimeLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(aboutUptimeLbl, Palette::textMuted(), 0);

        addUpdateControls(card);
        addDemoControls(card);

        return card;
    }

    // ---- category ring ----
    void showRing()
    {
        // A finished check is only meaningful while you're looking at it --
        // reopening About half an hour later shouldn't greet you with
        // "You're up to date" that was true on a different network.
        // dismiss() ignores this while an update is actually in flight.
        if (openPanel == 3) OtaUpdater::dismiss();
        if (openPanel >= 0) lv_obj_add_flag(panels[openPanel], LV_OBJ_FLAG_HIDDEN);
        openPanel = -1;
        ring.setVisible(true);
        lv_obj_clear_flag(hub, LV_OBJ_FLAG_HIDDEN);
    }

    void openCategory(int index)
    {
        if (index < 0 || index >= CATEGORY_COUNT) return;
        ring.setVisible(false);
        lv_obj_add_flag(hub, LV_OBJ_FLAG_HIDDEN);
        openPanel = index;
        lv_obj_clear_flag(panels[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(panels[index], 0, LV_ANIM_OFF);
    }

    void refreshHub(int index)
    {
        lv_label_set_text(hubNameLbl, CATEGORY_NAMES[index]);
    }

    void hubTapCb(lv_event_t *e)
    {
        (void)e;
        ring.openSelected();
    }

    void backBtnCb(lv_event_t *e)
    {
        (void)e;
        // Same button serves both levels: step out of a category first, and
        // only leave Settings once the ring is what's showing.
        if (openPanel >= 0) showRing();
        else UiNav::goHome();
    }

    void onItemStyle(lv_obj_t *chip, int i, float nearness)
    {
        lv_opa_t mix = (lv_opa_t)(255 * nearness);
        lv_obj_set_style_bg_color(chip, lv_color_mix(Palette::accent(), Palette::bgSecondary(), mix), 0);
        lv_obj_t *icon = lv_obj_get_child(chip, 0);
        if (!icon) return;
        lv_obj_set_style_text_color(icon, lv_color_mix(Palette::accentFg(), Palette::textMuted(), mix), 0);

        // A font change forces a label relayout, unlike the colour write
        // above -- skip it unless the size bucket actually flipped, since
        // this runs for every chip on every animation frame.
        UiRingIconSize want = uiRingIconSize(nearness, iconSize[i]);
        if (want != iconSize[i])
        {
            iconSize[i] = want;
            lv_obj_set_style_text_font(icon, uiRingIconFont(want), 0);
        }
    }

    lv_obj_t *makeChip(const char *icon)
    {
        lv_obj_t *chip = lv_obj_create(screenRoot);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_shadow_width(chip, 12, 0);
        lv_obj_set_style_shadow_color(chip, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(chip, LV_OPA_30, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_set_ext_click_area(chip, 10);

        lv_obj_t *lbl = lv_label_create(chip);
        lv_label_set_text(lbl, icon);
        // Must match iconSize[]'s initial value -- onItemStyle only writes a
        // font when the bucket CHANGES, so a mismatch here leaves chips
        // drawn at the wrong size until they happen to cross a band.
        lv_obj_set_style_text_font(lbl, uiRingIconFont(UiRingIconSmall), 0);
        lv_obj_center(lbl);
        return chip;
    }
}

lv_obj_t *uiSettingsCreate()
{
    screenRoot = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screenRoot, Palette::bgApp(), 0);
    // The screen itself never scrolls -- everything is placed by hand -- so
    // suppress any scrollbar it might otherwise draw over the UI.
    lv_obj_clear_flag(screenRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screenRoot, LV_SCROLLBAR_MODE_OFF);

    // Panels first so the ring and hub end up drawn above them.
    panels[0] = makeWifiCard();
    panels[1] = makeMachineCard();
    panels[2] = makeDisplayCard();
    panels[3] = makeAboutCard();

    hub = lv_obj_create(screenRoot);
    lv_obj_set_size(hub, 82, 82);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, Palette::bgSecondary(), 0);
    lv_obj_set_style_bg_color(hub, Palette::accentHover(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hub, Palette::border(), 0);
    lv_obj_set_style_border_width(hub, 1, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hub, hubTapCb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(hub);

    hubNameLbl = lv_label_create(hub);
    lv_obj_set_style_text_font(hubNameLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hubNameLbl, Palette::text(), 0);
    lv_obj_align(hubNameLbl, LV_ALIGN_CENTER, 0, -8);

    hubHintLbl = lv_label_create(hub);
    lv_label_set_text(hubHintLbl, "open");
    lv_obj_set_style_text_font(hubHintLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hubHintLbl, Palette::accent(), 0);
    lv_obj_align(hubHintLbl, LV_ALIGN_CENTER, 0, 12);

    // Arc rather than a full circle, same reason as Jobs: on a full circle
    // four items land at 12/3/6/9 o'clock, and the 6 o'clock one sits right
    // on top of the back button below.
    //
    // 40-degree pitch keeps all four inside the +/-132 arc at every
    // selection, so nothing is ever hidden -- unlike Jobs, this is a short
    // fixed menu and you want to see the whole thing. The spread pushes the
    // furthest one out to ~126 degrees, still inside the arc. opaFar stays
    // at 110 (not transparent like Jobs) so it also stays legible there
    // instead of fading out.
    ring.create(screenRoot, RING_RADIUS, RING_SIZE_NEAR, RING_SIZE_FAR, LV_OPA_COVER, 110);
    ring.setArcLayout(40.0f, 132.0f);
    ring.setSpread(RING_SPREAD);
    ring.setOnOpen(openCategory);
    ring.setOnSelect(refreshHub);
    ring.setOnItemStyle(onItemStyle);
    for (int i = 0; i < CATEGORY_COUNT; i++) ring.addItem(makeChip(CATEGORY_ICONS[i]));

    lv_obj_move_foreground(hub);
    refreshHub(0);

    // Custom back button rather than addBackButton(): this one has to step
    // out of an open category before it leaves the screen.
    lv_obj_t *back = lv_btn_create(screenRoot);
    lv_obj_set_size(back, 36, 36);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Palette::bgSecondary(), 0);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(back, backBtnCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LUCIDE_CHEVRON_LEFT);
    lv_obj_set_style_text_font(backLbl, &lucide_16, 0);
    lv_obj_set_style_text_color(backLbl, Palette::textMuted(), 0);
    lv_obj_center(backLbl);

    return screenRoot;
}

void uiSettingsHandleRotate(int32_t delta)
{
    if (delta == 0) return;
    if (scanOverlay)
    {
        // The network list sits on the top layer over whatever category is
        // open, so it has to claim the knob first -- otherwise turning it
        // scrolls the hidden Wi-Fi card underneath. Clamped, not wrapped:
        // the list is short and Rescan is a natural end stop.
        int count = scanItemCount();
        scanSel += (int)delta;
        if (scanSel < 0) scanSel = 0;
        if (scanSel >= count) scanSel = count - 1;
        styleScanSelection();
        return;
    }
    if (openPanel >= 0)
    {
        // Inside a category the knob scrolls its controls -- several of the
        // panels are taller than the round-safe area.
        lv_obj_scroll_by(panels[openPanel], 0, -delta * 24, LV_ANIM_ON);
        return;
    }
    for (int32_t i = 0; i < delta; i++) ring.selectNext();
    for (int32_t i = 0; i < -delta; i++) ring.selectPrev();
}

void uiSettingsHandleClick()
{
    if (scanOverlay)
    {
        if (scanSel < (int)lv_obj_get_child_cnt(scanList)) pickNetwork(lv_obj_get_child(scanList, scanSel));
        else runScan();
        return;
    }
    if (openPanel >= 0) return; // controls inside a panel are touch-operated
    ring.openSelected();
}

int uiSettingsSelectedCategory() { return ring.selectedIndex(); }

bool uiSettingsHandleBack()
{
    if (scanOverlay)
    {
        // Same as the overlay's close button: back out and reconnect.
        closeScanOverlay();
        reconnectAfterWifiEdit();
        return true;
    }
    if (openPanel < 0) return false;
    showRing();
    return true;
}

void uiSettingsOpenWifiSetup()
{
    // Wi-Fi is category 0, which is also the ring's default selection, so
    // backing out of the card lands on Wi-Fi in the ring.
    openCategory(0);
    openScanOverlay();
}

void uiSettingsUpdate()
{
    if (!wifiStatusLbl) return;

    wl_status_t status = WiFi.status();
    lv_label_set_text(wifiStatusLbl, status == WL_CONNECTED ? "Connected" : "Not connected");

    char buf[32];
    if (status == WL_CONNECTED) snprintf(buf, sizeof(buf), "IP: %s", WiFi.localIP().toString().c_str());
    else snprintf(buf, sizeof(buf), "IP: --");
    lv_label_set_text(aboutIpLbl, buf);

    uint32_t upSec = millis() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %luh %lum", (unsigned long)(upSec / 3600), (unsigned long)((upSec / 60) % 60));
    lv_label_set_text(aboutUptimeLbl, buf);

    refreshUpdateWidgets();
    refreshTpStatus();
}
