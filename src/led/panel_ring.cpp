#include "panel_ring.h"
#include <Arduino.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include "pins.h"

PanelRing panelRing;

#if BOARD_HAS_LED_RING || BOARD_HAS_SCREEN_RING

#if BOARD_HAS_LED_RING
static Adafruit_NeoPixel strip(LED_RING_COUNT, PIN_LED_RING, NEO_GRB + NEO_KHZ800);
#else
// No LEDs on this board: the same animations, drawn round the edge of the
// display instead (see screen_ring.h). Same interface, same code below.
#include "screen_ring.h"
static ScreenRing strip(LED_RING_COUNT);
#endif

// A ring of many "LEDs" (the on-screen one) gets a few touches a 5-LED
// ring can't use: a faster chase step, so a lap takes about as long as on
// 5, and a short comet tail behind the chasing light. Compile-time, so the
// 5-LED build is exactly what it was.
static const bool DENSE_RING = LED_RING_COUNT >= 12;
static const uint32_t CHASE_STEP_MS = DENSE_RING ? 50 : 120;
static uint8_t sineTab[256];

// Same durations terraPixel uses for its "just finished a job" celebration.
static const uint32_t CELEBRATE_MS = 12000;
static const uint32_t FRAME_MS = 33; // ~30fps -- was 20 (50fps); this is decorative, not worth the extra strip.show() calls (each briefly disables interrupts to bit-bang WS2812 timing)

static uint8_t sin8t(uint8_t theta) { return sineTab[theta]; }

// Oscillate between lo and hi at the given bpm -- ported from terraPixel's beat().
static uint8_t beat(uint8_t bpm, uint8_t lo, uint8_t hi)
{
    uint32_t phase = (millis() * bpm * 256UL) / 60000UL;
    uint16_t v = sin8t(phase & 0xFF);
    return lo + ((uint32_t)v * (hi - lo)) / 255;
}

static void buildSineTable()
{
    for (int i = 0; i < 256; i++)
        sineTab[i] = (uint8_t)(127.5 + 127.4 * sin(i * 2.0 * PI / 256.0));
}

void PanelRing::begin()
{
    if (!sineTableBuilt_)
    {
        buildSineTable();
        sineTableBuilt_ = true;
    }
    strip.begin();
    strip.setBrightness((brightnessPct_ * 255) / 100);
    strip.clear();
    strip.show();
    modeEnteredAt_ = millis();
}

void PanelRing::setMode(MachineMode mode)
{
    if (mode == mode_) return;
    mode_ = mode;
    modeEnteredAt_ = millis();
}

void PanelRing::setBrightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    brightnessPct_ = percent;
}

void PanelRing::fillAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_RING_COUNT; i++) strip.setPixelColor(i, r, g, b);
}

void PanelRing::render()
{
    uint32_t t = millis();
    uint8_t userBright = (brightnessPct_ * 255) / 100;

    switch (mode_)
    {
        case MachineMode::Run:
        {
            if (plotting_)
            {
                // A plot: the whole ring lit, breathing slowly, for however
                // long the job takes. The chase below is the right read for
                // a jog and the wrong one for an hour-long plot sitting in
                // your peripheral vision -- motion is what the eye keeps
                // going back to, and one pixel racing round a dark ring is
                // nearly all motion.
                //
                // Scaled INSIDE the user's brightness rather than driven at
                // absolute levels like the Hold/Alarm/Boot pulses. Two
                // reasons: the Display brightness setting should still mean
                // something during the longest thing the machine does, and
                // ScreenSleep dims this same value when the panel sleeps --
                // so the ring fades down with the screen and stays lit
                // through the night instead of blazing on at a fixed level.
                uint8_t lo = (uint8_t)(((uint32_t)userBright * 45) / 100);
                strip.setBrightness(beat(16, lo, userBright)); // ~3.75s per breath
                fillAll(255, 250, 240);
                break;
            }

            // No position feed on this ring (unlike terraPixel's rail comet) --
            // a single bright pixel chases around the 5 LEDs to read as "active".
            strip.setBrightness(userBright);
            fillAll(0, 0, 0);
            int step = (int)((t / CHASE_STEP_MS) % LED_RING_COUNT);
            // Confirmed on hardware: this strip's pixel order runs
            // COUNTER-clockwise around the panel, so ascending index walks
            // the ring backwards relative to the knob. Hence clockwise
            // (chaseDir_ >= 0) is the descending-index case, not the
            // ascending one -- the obvious mapping had the light sweeping
            // opposite the direction the dial was turned.
            int idx = chaseDir_ >= 0 ? (LED_RING_COUNT - 1 - step) : step;
            strip.setPixelColor(idx, 255, 250, 240);
            if (DENSE_RING)
            {
                // The tail trails behind the direction of travel: clockwise
                // is descending index (see above), so behind is ascending.
                int behind = chaseDir_ >= 0 ? 1 : LED_RING_COUNT - 1;
                int t1 = (idx + behind) % LED_RING_COUNT;
                int t2 = (t1 + behind) % LED_RING_COUNT;
                strip.setPixelColor(t1, 102, 100, 96);
                strip.setPixelColor(t2, 38, 37, 36);
            }
            break;
        }

        case MachineMode::Done:
            if (t - modeEnteredAt_ > CELEBRATE_MS) { mode_ = MachineMode::Idle; modeEnteredAt_ = t; break; }
            strip.setBrightness(beat(30, 90, 255));
            fillAll(0, 255, 60);
            break;

        case MachineMode::Homing:
        {
            strip.setBrightness(userBright);
            const uint32_t SWEEP_MS = 400, HOLD_MS = 120, GAP_MS = 120;
            const uint32_t CYCLE_MS = SWEEP_MS + HOLD_MS + GAP_MS;
            uint32_t phase = t % CYCLE_MS;
            int lit;
            if (phase < SWEEP_MS) lit = (int)((phase * (uint32_t)LED_RING_COUNT) / SWEEP_MS);
            else if (phase < SWEEP_MS + HOLD_MS) lit = LED_RING_COUNT;
            else lit = -1;
            for (int i = 0; i < LED_RING_COUNT; i++)
                strip.setPixelColor(i, i < lit ? 255 : 0, i < lit ? 90 : 0, 0);
            break;
        }

        case MachineMode::Hold:
            strip.setBrightness(beat(60, 60, 200));
            fillAll(255, 130, 0);
            break;

        case MachineMode::Alarm:
            strip.setBrightness(((t / 300) % 2) ? 200 : 20);
            fillAll(255, 0, 0);
            break;

        case MachineMode::Boot:
            strip.setBrightness(beat(20, 10, 50));
            fillAll(0, 40, 255);
            break;

        case MachineMode::Idle:
        default:
            strip.setBrightness(userBright);
            for (int i = 0; i < LED_RING_COUNT; i++)
            {
                uint8_t v = sin8t((i * (256 / LED_RING_COUNT)) + (t / 24)); // one wave spread evenly round the ring (51 apart on 5 LEDs)
                strip.setPixelColor(i, 40 + (v >> 2), 22 + (v >> 3), 4);
            }
            break;
    }
}

void PanelRing::update()
{
    uint32_t now = millis();
    if (now - lastFrameAt_ < FRAME_MS) return;
    lastFrameAt_ = now;
    render();
    strip.show();
}

#else

// No ring of either kind on this board (pins.h). The object still exists so callers don't
// need #ifs, and still remembers its mode and brightness -- the Lights
// screen reads brightness() back -- it just never lights anything.
void PanelRing::begin() {}

void PanelRing::setMode(MachineMode mode) { mode_ = mode; }

void PanelRing::setBrightness(uint8_t percent) { brightnessPct_ = percent > 100 ? 100 : percent; }

void PanelRing::update() {}

#endif
