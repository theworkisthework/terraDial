#include "encoder.h"
#include <Arduino.h>
#include "pins.h"
#include "haptics.h"

JogWheel jogWheel;

namespace
{
    // Both decoders below feed the same signed tick count; everything
    // after that (detents, the reversal fix, press suppression) is shared.
    volatile int32_t qRawTicks = 0;

#if BOARD_ENCODER_QUADRATURE
    // Interrupt-driven quadrature decode, standard 2-bit state-transition
    // table: index = (prevState<<2)|newState, each state is (A<<1)|B.
    // Invalid/bounce transitions (both bits appearing to change at once)
    // map to 0 and are ignored rather than guessed at.
    volatile int8_t qLastState = 0;

    const int8_t QUAD_TABLE[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0
    };

    void IRAM_ATTR onEncoderChange()
    {
        uint8_t a = digitalRead(PIN_ENCODER_A);
        uint8_t b = digitalRead(PIN_ENCODER_B);
        uint8_t newState = (a << 1) | b;
        uint8_t idx = ((uint8_t)qLastState << 2) | newState;
        qRawTicks += QUAD_TABLE[idx & 0x0F];
        qLastState = newState;
    }

    // This EC11-style mechanical encoder latches at one detent per full
    // 4-tick quadrature cycle (the common case for these panel jog wheels).
    const int32_t TICKS_PER_DETENT = 4;
#else
    // "Bidirectional switch" knob (Waveshare Knob): not quadrature. Each
    // detent pulls ONE line low and lets it go -- A for one direction, B
    // for the other -- so a pulse on A is +1 and a pulse on B is -1, and
    // the order of edges between the two lines means nothing. This is what
    // Waveshare's own decoder does (04_Encoder_Test/bidi_switch_knob.c);
    // feeding these pins to the quadrature table above would read noise.
    //
    // Sampled on a fixed 3ms timer, NOT edge interrupts, and decoded exactly
    // as Waveshare's decoder does it -- same period, same debounce rule.
    //
    // The first version here was interrupt-driven (count a rising edge that
    // followed >= 2ms of low), and on the hardware it dropped clicks, more
    // of them one way than the other. An edge interrupt has to read the pin
    // to learn which way it went, and on a slow or ringing edge that read
    // can still see LOW just after the line rose: the "rise" is then taken
    // for a fresh fall, the low timer restarts, and the real rise a moment
    // later looks like a sub-millisecond glitch and is thrown away. Steady
    // sampling can't be fooled that way -- it only ever compares one settled
    // reading with the next.
    //
    // The rule, per line: a pulse scores on the sample where the line reads
    // high again, provided it read low on at least DEBOUNCE_SAMPLES samples
    // in a row beforehand (two samples is 3-6ms of low). Shorter dips are
    // contact bounce.
    const uint32_t SAMPLE_US = 3000;
    const uint8_t DEBOUNCE_SAMPLES = 2;

    struct PulseLine
    {
        uint8_t pin;
        int8_t dir;
        uint8_t prevLevel;
        uint8_t lowCount;
    };

    // Which line is "clockwise" is from Waveshare's decoder (A = KNOB_RIGHT).
    // If the knob turns out backwards on the hardware, swap the signs here.
    PulseLine pulseLines[2] = {
        {PIN_ENCODER_A, +1, 1, 0},
        {PIN_ENCODER_B, -1, 1, 0},
    };

    void samplePulseLine(PulseLine &l)
    {
        uint8_t level = digitalRead(l.pin);
        if (level == LOW)
        {
            // Count consecutive low samples, starting over on each new fall.
            l.lowCount = (l.prevLevel == LOW && l.lowCount < 255) ? l.lowCount + 1 : 1;
        }
        else if (l.prevLevel == LOW)
        {
            if (l.lowCount >= DEBOUNCE_SAMPLES) qRawTicks += l.dir;
            l.lowCount = 0;
        }
        l.prevLevel = level;
    }

    // esp_timer callback: runs on the esp_timer task, not in an ISR.
    // qRawTicks is only ever written here, and an aligned 32-bit store is
    // atomic, so updateRotation()'s read needs nothing more.
    void onSampleTimer(void *)
    {
        samplePulseLine(pulseLines[0]);
        samplePulseLine(pulseLines[1]);
    }

    // One pulse is one detent.
    const int32_t TICKS_PER_DETENT = 1;
#endif
}

void JogWheel::begin()
{
#if BOARD_ENCODER_QUADRATURE
    pinMode(PIN_ENCODER_A, INPUT);
    pinMode(PIN_ENCODER_B, INPUT);

    qLastState = (digitalRead(PIN_ENCODER_A) << 1) | digitalRead(PIN_ENCODER_B);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), onEncoderChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B), onEncoderChange, CHANGE);
#else
    // The board has its own 10k pull-ups; these just make sure.
    pinMode(PIN_ENCODER_A, INPUT_PULLUP);
    pinMode(PIN_ENCODER_B, INPUT_PULLUP);
    for (PulseLine &l : pulseLines) l.prevLevel = digitalRead(l.pin);

    const esp_timer_create_args_t args = {
        .callback = onSampleTimer,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "knob",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, SAMPLE_US);
    else Serial.println("[knob] couldn't start the sampling timer -- knob disabled");
#endif

#if BOARD_HAS_KNOB_BUTTON
    pinMode(PIN_ENCODER_SW, INPUT_PULLUP);
#endif
}

void JogWheel::update()
{
    updateRotation();
#if BOARD_HAS_KNOB_BUTTON
    updateButton();
#endif
}

void JogWheel::updateRotation()
{
    noInterrupts();
    int32_t ticks = qRawTicks;
    interrupts();

    int32_t diff = ticks - lastRawTicks_;
    lastRawTicks_ = ticks;

    // Throw away the sub-detent remainder when the direction reverses.
    //
    // This is what made the first turn back after a change of direction do
    // nothing (reported as needing "two or more turns" to reverse). A
    // detented wheel always comes to rest ON a latch, so a non-zero
    // pendingTicks_ isn't real travel that's owed to us -- it's accumulated
    // error, from the ISR reading both pins already changed on a fast spin
    // and scoring that transition 0 instead of 2. Carried forward, that
    // error is signed: a remainder of +2 left over from turning clockwise
    // has to be paid off before a counter-clockwise detent can register, so
    // the first turn back costs 6 ticks instead of 4 and looks ignored.
    // Worse, it never settles -- the leftover just flips sign and taxes the
    // NEXT reversal too.
    //
    // Same-direction turning is untouched: the remainder is only discarded
    // when its sign opposes the new movement.
    if ((diff > 0 && pendingTicks_ < 0) || (diff < 0 && pendingTicks_ > 0)) pendingTicks_ = 0;

    pendingTicks_ += diff;

    int32_t detents = pendingTicks_ / TICKS_PER_DETENT;
    pendingTicks_ -= detents * TICKS_PER_DETENT;
    rotationAccum_ += detents;

    if (detents != 0)
    {
        lastActivityAt_ = millis();
        Haptics::detent(); // one tick however many detents landed: a fast spin buzzes, not stutters
    }
}

// Never called on a board without a knob button (see update()), so
// takeButtonEvent() there always reports None.
void JogWheel::updateButton()
{
#if BOARD_HAS_KNOB_BUTTON
    uint32_t now = millis();
    bool rawLow = digitalRead(PIN_ENCODER_SW) == LOW;

    if (rawLow != rawLevelLow_)
    {
        rawLevelLow_ = rawLow;
        levelSince_ = now;
    }
    else if (now - levelSince_ >= DEBOUNCE_MS)
    {
        // Stable reading -- act on transitions into/out of "pressed".
        if (rawLow && !pressed_)
        {
            pressed_ = true;
            pressedAt_ = now;
            longPressFired_ = false;
            suppressPress_ = false;
            pressStartRawTicks_ = lastRawTicks_;
            lastActivityAt_ = now;
        }
        else if (!rawLow && pressed_)
        {
            pressed_ = false;
            if (!longPressFired_ && !suppressPress_)
            {
                pendingClicks_++;
                lastReleaseAt_ = now;
            }
        }
    }

    int32_t movedTicks = lastRawTicks_ - pressStartRawTicks_;
    if (movedTicks < 0) movedTicks = -movedTicks;
    if (pressed_ && !suppressPress_ && movedTicks > PRESS_MOVEMENT_SUPPRESS_TICKS)
    {
        suppressPress_ = true;
    }

    if (pressed_ && !suppressPress_ && !longPressFired_ && (now - pressedAt_ >= LONG_PRESS_MS))
    {
        longPressFired_ = true;
        pendingClicks_ = 0; // long press pre-empts any pending click/double-click
        pendingEvent_ = ButtonEvent::LongPress;
    }

    if (pendingClicks_ > 0 && !pressed_ && (now - lastReleaseAt_ > DOUBLE_CLICK_MS))
    {
        pendingEvent_ = (pendingClicks_ >= 2) ? ButtonEvent::DoubleClick : ButtonEvent::Click;
        pendingClicks_ = 0;
    }
#endif
}

int32_t JogWheel::takeRotationDelta()
{
    int32_t v = rotationAccum_;
    rotationAccum_ = 0;
    return v;
}

ButtonEvent JogWheel::takeButtonEvent()
{
    ButtonEvent e = pendingEvent_;
    pendingEvent_ = ButtonEvent::None;
    return e;
}
