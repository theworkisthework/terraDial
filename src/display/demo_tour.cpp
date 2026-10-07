#include "demo_tour.h"
#include "ui_scale.h"
#include <Arduino.h>
#include "../net/demo_mode.h"
#include "../net/fluidnc_client.h"
#include "../net/terrapixel_client.h"
#include "../config/settings.h"
#include "ui_nav.h"
#include "ui_dial.h"
#include "ui_files.h"
#include "ui_settings.h"
#include "radial_keyboard.h"
#include "screen_sleep.h"

namespace
{
    // How long the panel must be left alone before the tour starts. Short
    // the first time after demo mode is switched on -- whoever flicked the
    // switch wants to see the tour, and 20s of nothing read as the panel
    // hanging -- and long once it has run: by then a pause means a visitor
    // is using the panel, and the tour shouldn't snatch it back mid-poke.
    const uint32_t TOUR_FIRST_IDLE_MS = 3000;
    const uint32_t TOUR_IDLE_MS = 20000;
    // Gap between the tour's knob detents -- slow enough to follow.
    const uint32_t DETENT_MS = 450;

    // Dial items, in ui_dial.cpp's DIAL_ITEMS order.
    enum DialItem { HOME_XY, JOG, PEN, JOBS, PHOTO, ESTOP, LIGHTS, SETTINGS };
    // Settings categories, in ui_settings.cpp's order.
    enum SettingsCat { CAT_WIFI, CAT_MACHINE, CAT_DISPLAY, CAT_ABOUT };

    enum class Op : uint8_t
    {
        Dial,       // go to the dial and turn to item `arg`
        Category,   // turn the Settings ring to category `arg`
        MenuTurn,   // `arg` detents on a menu (honours "invert menu rotation")
        RawTurn,    // `arg` raw detents (Jog: machine direction, never inverted)
        Click,
        LongPress,
        Call,       // run `fn`
        Wait,
    };

    struct Step
    {
        Op op;
        int8_t arg;
        uint16_t pauseMs; // after the step (and its detents) finish
        void (*fn)();
    };

    // ---- actions with no knob equivalent ----
    void clearAlarmIfAlarmed()
    {
        if (fluidNC.status().mode == MachineMode::Alarm) fluidNC.clearAlarm();
    }
    void goDial() { UiNav::goHome(); }
    void lightsBright() { terraPixel.setBrightness(255); }
    void lightsDim() { terraPixel.setBrightness(120); }
    void lightsParty() { terraPixel.toggleParty(); }
    void lightsFilm() { terraPixel.setFilmMode(true); }
    void lightsNormal() { terraPixel.setFilmMode(false); }

    // The tour. The demo is deterministic -- the simulated machine and SD
    // card behave the same every time -- so a timed script is enough; the
    // Dial/Category ops pick targets by name rather than counting turns,
    // so a visitor leaving a menu somewhere odd can't knock it off course.
    const Step SCRIPT[] = {
        // Settle: tour start may have soft-reset a running job into ALARM.
        {Op::Wait, 0, 900, nullptr},
        {Op::Call, 0, 700, clearAlarmIfAlarmed},
        {Op::Call, 0, 1500, goDial},

        // The dial: one slow turn all the way round.
        {Op::MenuTurn, 8, 1500, nullptr},

        // Home XY: the clear-the-bed screen, confirm, watch it home.
        {Op::Dial, HOME_XY, 900, nullptr},
        {Op::Click, 0, 2500, nullptr},
        {Op::Click, 0, 4500, nullptr}, // confirm: homes, back to the dial

        // Jog: X out, switch axis, Y back.
        {Op::Dial, JOG, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::RawTurn, 5, 1200, nullptr},
        {Op::Click, 0, 1000, nullptr}, // next axis
        {Op::RawTurn, -4, 1500, nullptr},
        {Op::LongPress, 0, 1000, nullptr},

        // Pen: down, up.
        {Op::Dial, PEN, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::Click, 0, 2000, nullptr},
        {Op::Click, 0, 2000, nullptr},
        {Op::LongPress, 0, 1000, nullptr},

        // Jobs: browse (the long name scrolls), open a folder and back out,
        // then run a job and follow it through Job Progress to the end.
        {Op::Dial, JOBS, 900, nullptr},
        {Op::Click, 0, 2000, nullptr},
        {Op::MenuTurn, 1, 1200, nullptr},
        {Op::MenuTurn, 1, 1200, nullptr},
        {Op::MenuTurn, 2, 6000, nullptr},  // the long filename
        {Op::MenuTurn, -4, 1000, nullptr}, // back to "Portraits"
        {Op::Click, 0, 2500, nullptr},     // open it
        {Op::MenuTurn, 1, 1500, nullptr},
        {Op::LongPress, 0, 2000, nullptr}, // up to the SD root
        {Op::MenuTurn, 2, 1500, nullptr},  // snowflake.gcode
        {Op::Click, 0, 8000, nullptr},     // run -- Job Progress opens itself
        {Op::Click, 0, 3000, nullptr},     // pause
        {Op::Click, 0, 1000, nullptr},     // resume
        // The 45s job has had ~17s by here; this sees it out with margin,
        // so the Photo steps don't start while it's still running.
        {Op::Wait, 0, 40000, nullptr},     // runs out, DONE, back to the dial

        // Photo: park the head for the camera.
        {Op::Dial, PHOTO, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::Click, 0, 9000, nullptr},
        {Op::LongPress, 0, 1000, nullptr},

        // Lights: brightness, film mode, party.
        {Op::Dial, LIGHTS, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::Call, 0, 1500, lightsBright},
        {Op::Call, 0, 2000, lightsFilm},
        {Op::Call, 0, 1500, lightsNormal},
        {Op::Call, 0, 3000, lightsParty},
        {Op::Call, 0, 1000, lightsParty},
        {Op::Call, 0, 1000, lightsDim},
        {Op::MenuTurn, 3, 1500, nullptr},
        {Op::MenuTurn, -3, 800, nullptr},
        {Op::LongPress, 0, 1000, nullptr},

        // E-Stop: start a job, stop it mid-run, clear the alarm.
        {Op::Dial, JOBS, 900, nullptr},
        {Op::Click, 0, 2000, nullptr},
        {Op::MenuTurn, 3, 1200, nullptr},  // spirograph_rose.gcode
        {Op::Click, 0, 6000, nullptr},     // run
        {Op::LongPress, 0, 1000, nullptr}, // leave it running
        {Op::Dial, ESTOP, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::Click, 0, 4000, nullptr},     // stop -- the alarm screen opens itself
        {Op::Click, 0, 2500, nullptr},     // clear it
        {Op::Call, 0, 1200, goDial},

        // Settings: the category ring, then About.
        {Op::Dial, SETTINGS, 900, nullptr},
        {Op::Click, 0, 1500, nullptr},
        {Op::Category, CAT_WIFI, 1000, nullptr},
        {Op::Category, CAT_DISPLAY, 1000, nullptr},
        {Op::Category, CAT_ABOUT, 1200, nullptr},
        {Op::Click, 0, 2000, nullptr},
        {Op::MenuTurn, 4, 2500, nullptr},  // scroll down the About page
        {Op::MenuTurn, 4, 2500, nullptr},
        {Op::LongPress, 0, 1000, nullptr}, // close About
        {Op::LongPress, 0, 1500, nullptr}, // back to the dial
    };
    const int SCRIPT_LEN = sizeof(SCRIPT) / sizeof(SCRIPT[0]);

    bool running = false;
    bool demoWasOn = false;
    bool startedSinceOn = false; // has the tour run since demo was switched on?
    uint32_t lastRealInputAt = 0;

    int stepIndex = 0;
    uint32_t nextStepAt = 0;
    // Output queued for takeInput(): detents still to deliver, and a click.
    int32_t pendingDetents = 0;
    uint32_t nextDetentAt = 0;
    ButtonEvent pendingEvent = ButtonEvent::None;

    // A menu turn in UI terms, as the raw knob delta that produces it.
    int32_t menuToRaw(int32_t steps) { return Config::get().invertMenuRotation ? -steps : steps; }

    // Shortest way round a full ring of `count`.
    int32_t ringSteps(int from, int to, int count)
    {
        int32_t d = ((to - from) % count + count) % count;
        return d > count / 2 ? d - count : d;
    }

    void start()
    {
        running = true;
        stepIndex = 0;
        nextStepAt = millis();
        pendingDetents = 0;
        pendingEvent = ButtonEvent::None;
        Serial.println("[demo] tour starting");

        // Wake, and clear away whatever a visitor left open.
        ScreenSleep::noteInputAndWake();
        if (RadialKeyboard::isOpen()) RadialKeyboard::handleLongPress();
        while (uiSettingsHandleBack()) {}
        while (uiFilesHandleBack()) {}

        // A job or move still running would be refused a new one; stop it.
        // (In demo that's the simulator -- nothing real is attached.)
        MachineMode m = fluidNC.status().mode;
        if (m == MachineMode::Run || m == MachineMode::Hold || m == MachineMode::Homing)
            fluidNC.softReset();
        UiNav::goHome();
    }

    void stop()
    {
        running = false;
        pendingDetents = 0;
        pendingEvent = ButtonEvent::None;
        Serial.println("[demo] tour stopped -- someone's using the panel");
    }

    void runStep(const Step &s)
    {
        int32_t detents = 0;
        switch (s.op)
        {
            case Op::Dial:
                if (!UiNav::isOnDial()) UiNav::goHome();
                detents = menuToRaw(ringSteps(uiDialSelectedIndex(), s.arg, 8));
                break;
            case Op::Category:
                detents = menuToRaw(ringSteps(uiSettingsSelectedCategory(), s.arg, 4));
                break;
            case Op::MenuTurn: detents = menuToRaw(s.arg); break;
            case Op::RawTurn: detents = s.arg; break;
            case Op::Click: pendingEvent = ButtonEvent::Click; break;
            case Op::LongPress: pendingEvent = ButtonEvent::LongPress; break;
            case Op::Call: if (s.fn) s.fn(); break;
            case Op::Wait: break;
        }
        uint32_t now = millis();
        pendingDetents = detents;
        nextDetentAt = now;
        nextStepAt = now + (uint32_t)abs(detents) * DETENT_MS + s.pauseMs;
    }
}

namespace DemoTour
{
    void noteRealInput()
    {
        lastRealInputAt = millis();
        if (running) stop();
    }

    bool isRunning() { return running; }

    void update()
    {
        bool demo = Demo::isOn();
        if (!demo)
        {
            if (running) stop();
            demoWasOn = false;
            return;
        }
        uint32_t now = millis();
        if (!demoWasOn)
        {
            // Just switched on (by hand): the idle clock starts now, so the
            // tour doesn't jump in under the finger that flicked the switch.
            demoWasOn = true;
            startedSinceOn = false;
            lastRealInputAt = now;
        }

        if (!running)
        {
            uint32_t idleNeeded = startedSinceOn ? TOUR_IDLE_MS : TOUR_FIRST_IDLE_MS;
            if (now - lastRealInputAt < idleNeeded) return;
            startedSinceOn = true;
            start();
        }

        ScreenSleep::keepAwake(); // no idle logo or sleep mid-tour

        if (pendingDetents != 0 || pendingEvent != ButtonEvent::None) return; // still delivering
        if ((int32_t)(now - nextStepAt) < 0) return;

        runStep(SCRIPT[stepIndex]);
        stepIndex = (stepIndex + 1) % SCRIPT_LEN;
    }

    void takeInput(int32_t &delta, ButtonEvent &ev)
    {
        if (!running) return;
        uint32_t now = millis();
        if (pendingDetents != 0 && (int32_t)(now - nextDetentAt) >= 0)
        {
            int32_t one = pendingDetents > 0 ? 1 : -1;
            delta = one;
            pendingDetents -= one;
            nextDetentAt = now + DETENT_MS;
            return; // a turn this call; any click waits for the next
        }
        if (pendingDetents == 0 && pendingEvent != ButtonEvent::None)
        {
            ev = pendingEvent;
            pendingEvent = ButtonEvent::None;
        }
    }
}
