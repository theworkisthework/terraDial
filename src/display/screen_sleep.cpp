#include "screen_sleep.h"
#include "ui_scale.h"
#include <Arduino.h>
#include "lgfx_config.h" // backlightSet()
#include "../config/settings.h"
#include "ui_brand.h"
#include "../led/panel_ring.h"
#include "../power/battery.h"

namespace
{
    bool asleep = false;
    uint32_t lastActivityAt = 0;

    // The ring brightness the user actually chose, saved on the way into
    // sleep so waking restores it rather than leaving the ring stuck at the
    // sleep level.
    uint8_t savedLedBrightness = 60;

    // How long before the brand screen appears. Deliberately much shorter
    // than the sleep timeout and not user-configurable: it's a display, not
    // a decision -- one toggle to disable it is enough.
    const uint32_t IDLE_LOGO_MS = 30000;

    // On battery the backlight is most of the drain, so the panel gives up
    // sooner: the idle logo shows dimmed, and sleep comes after a minute
    // whatever the setting says -- unless the setting is shorter, or is
    // "never", which is a choice someone made on purpose.
    const uint16_t BATTERY_SLEEP_SEC = 60;
    const uint8_t BATTERY_LOGO_BRIGHTNESS_PCT = 30;
    bool logoDimmed = false;

    bool onBattery()
    {
        const BatteryStatus &b = Battery::status();
        return b.present && !b.external;
    }

    uint16_t sleepAfterSec()
    {
        uint16_t sec = Config::get().sleepTimeoutSec;
        if (sec != 0 && onBattery() && sec > BATTERY_SLEEP_SEC) sec = BATTERY_SLEEP_SEC;
        return sec;
    }

    void undimLogo()
    {
        if (!logoDimmed) return;
        logoDimmed = false;
        backlightSet(Config::get().backlightBrightnessPct);
    }

    void goToSleep()
    {
        asleep = true;
        savedLedBrightness = panelRing.brightness();
        panelRing.setBrightness(Config::get().sleepLedBrightnessPct);
        backlightSet(0); // fully off, not dimmed -- the point is a dark panel
    }

    void wake()
    {
        UiBrand::hide(); // sleep is normally entered via the brand screen
        logoDimmed = false; // the full level is restored just below
        asleep = false;
        backlightSet(Config::get().backlightBrightnessPct);
        panelRing.setBrightness(savedLedBrightness);
        lastActivityAt = millis();
    }
}

namespace ScreenSleep
{
    void begin() { lastActivityAt = millis(); }

    bool isAsleep() { return asleep; }

    void update()
    {
        uint32_t idleMs = millis() - lastActivityAt;

        // Stage one: the brand screen. Runs on its own timer so it still
        // appears when sleep is switched off entirely -- an idle panel on
        // the machine may as well show the mark.
        if (!asleep && Config::get().showIdleLogo && idleMs > IDLE_LOGO_MS && !UiBrand::isShown())
        {
            UiBrand::show();
            if (onBattery() && Config::get().backlightBrightnessPct > BATTERY_LOGO_BRIGHTNESS_PCT)
            {
                backlightSet(BATTERY_LOGO_BRIGHTNESS_PCT);
                logoDimmed = true;
            }
        }

        uint16_t timeoutSec = sleepAfterSec();
        if (timeoutSec == 0)
        {
            // Sleep switched off while asleep (only reachable if it was
            // changed remotely, but cheap to handle) -- come straight back.
            if (asleep) wake();
            return;
        }
        if (!asleep && millis() - lastActivityAt > (uint32_t)timeoutSec * 1000UL) goToSleep();
    }

    void keepAwake() { lastActivityAt = millis(); }

    bool noteInputAndWake()
    {
        lastActivityAt = millis();

        // Both stages swallow the input that dismisses them. Grabbing a dark
        // or branded panel to see what's happening must never also press
        // whatever was underneath.
        bool dismissed = false;
        if (UiBrand::isShown())
        {
            UiBrand::hide();
            undimLogo();
            dismissed = true;
        }
        if (asleep)
        {
            wake();
            dismissed = true;
        }
        return dismissed;
    }
}
