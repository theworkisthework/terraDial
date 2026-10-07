#include "battery.h"
#include "pins.h"

namespace
{
    BatteryStatus st;
}

#if BOARD_HAS_BATTERY

#include <Arduino.h>

namespace
{
    const uint32_t SAMPLE_MS = 2000;
    const int SAMPLES_PER_READ = 8;

    // The rail sits at ~5V on USB and at the cell's voltage (4.2V at most)
    // on battery. Two thresholds, not one, so a rail hovering near the
    // boundary can't flicker the icon between the two.
    const uint16_t EXTERNAL_ABOVE_MV = 4450;
    const uint16_t BATTERY_BELOW_MV = 4350;

    // Single-cell LiPo, voltage at rest -> charge. Coarse by nature: the
    // curve is flat through the middle and sags under load (the backlight
    // and Wi-Fi are load), so this is a fuel gauge, not a meter.
    struct Point { uint16_t mv; uint8_t pct; };
    const Point CURVE[] = {
        {4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 50},
        {3750, 40}, {3700, 30}, {3650, 20}, {3600, 12}, {3500, 5}, {3300, 0},
    };
    const int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

    uint8_t percentFor(uint16_t mv)
    {
        if (mv >= CURVE[0].mv) return 100;
        for (int i = 1; i < CURVE_N; i++)
        {
            if (mv >= CURVE[i].mv)
            {
                const Point &hi = CURVE[i - 1], &lo = CURVE[i];
                return lo.pct + (uint32_t)(mv - lo.mv) * (hi.pct - lo.pct) / (hi.mv - lo.mv);
            }
        }
        return 0;
    }

    uint32_t lastSampleAt = 0;
    float smoothedMv = 0; // 0 = no reading yet

    void sample()
    {
        uint32_t sum = 0;
        for (int i = 0; i < SAMPLES_PER_READ; i++) sum += analogReadMilliVolts(PIN_BATT_ADC);
        float mv = (float)sum / SAMPLES_PER_READ * BATT_ADC_DIVIDER;

        // Smoothed, because the reading tracks the load: a Wi-Fi burst or
        // the backlight coming on dips it for a moment, and a raw reading
        // would walk the percentage up and down with them. The first
        // reading is taken as-is so boot doesn't show a climb from zero.
        smoothedMv = smoothedMv == 0 ? mv : smoothedMv * 0.8f + mv * 0.2f;
        st.millivolts = (uint16_t)smoothedMv;

        if (!st.external && st.millivolts > EXTERNAL_ABOVE_MV) st.external = true;
        else if (st.external && st.millivolts < BATTERY_BELOW_MV) st.external = false;

        // On USB the rail says nothing about the cell, so the last
        // on-battery estimate stands rather than reading 100%.
        if (!st.external) st.percent = percentFor(st.millivolts);
    }
}

namespace Battery
{
    void begin()
    {
        st.present = true;
        analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db); // full ~3.1V range: 5V/2 fits
        sample();
        lastSampleAt = millis();
        Serial.printf("[battery] %umV, %s\n", st.millivolts, st.external ? "on USB" : "on battery");
    }

    void update()
    {
        if (millis() - lastSampleAt < SAMPLE_MS) return;
        lastSampleAt = millis();
        sample();
    }

    const BatteryStatus &status() { return st; }
}

#else

namespace Battery
{
    void begin() {}
    void update() {}
    const BatteryStatus &status() { return st; }
}

#endif
