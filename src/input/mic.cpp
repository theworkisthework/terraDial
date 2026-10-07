#include "mic.h"
#include "pins.h"

#if BOARD_HAS_MIC

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

namespace
{
    // PDM microphone straight onto the S3 (Waveshare schematic sheet 4).
    // In PDM receive mode the I2S peripheral's WS pin is the mic clock.
    const i2s_port_t PORT = I2S_NUM_0; // the S3 only does PDM RX on I2S0
    const uint32_t SAMPLE_RATE = 16000;
    const int BLOCK = 256; // 16ms of audio per loudness reading

    // Loudness is judged against the room, not an absolute level: a
    // workshop with a fan running and a quiet study need different
    // thresholds. The floor drifts towards whatever is normal...
    const float FLOOR_FOLLOW = 0.02f;
    // ...and speech is anything this many times louder than it, and at
    // least VOICE_MIN outright, so a dead-silent room's floor of nearly
    // nothing doesn't make a breath count.
    const float VOICE_RATIO = 4.0f;
    const float VOICE_MIN = 300.0f;

    volatile uint32_t listenUntil = 0;
    volatile uint32_t lastVoiceAt = 0;
    volatile bool running = false; // I2S clocking the mic
    bool installed = false;

    void micTask(void *)
    {
        static int16_t buf[BLOCK];
        float floorLevel = VOICE_MIN / VOICE_RATIO;

        for (;;)
        {
            bool want = (int32_t)(listenUntil - millis()) > 0;
            if (want != running)
            {
                if (want) i2s_start(PORT);
                else i2s_stop(PORT);
                running = want;
                if (want) floorLevel = VOICE_MIN / VOICE_RATIO; // relearn the room
            }
            if (!running)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            size_t got = 0;
            if (i2s_read(PORT, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) continue;
            int n = got / sizeof(int16_t);

            // RMS about the block's own mean: PDM mics carry a DC offset
            // that would otherwise read as permanent loudness.
            float mean = 0;
            for (int i = 0; i < n; i++) mean += buf[i];
            mean /= n;
            float sq = 0;
            for (int i = 0; i < n; i++)
            {
                float v = buf[i] - mean;
                sq += v * v;
            }
            float rms = sqrtf(sq / n);

            if (rms > floorLevel * VOICE_RATIO && rms > VOICE_MIN)
                lastVoiceAt = millis();
            else
                floorLevel += (rms - floorLevel) * FLOOR_FOLLOW; // only learn from quiet
        }
    }
}

namespace Mic
{
    void begin()
    {
        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
        cfg.sample_rate = SAMPLE_RATE;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT; // the mic's L/R pin is tied low
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.dma_buf_count = 4;
        cfg.dma_buf_len = BLOCK;

        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = I2S_PIN_NO_CHANGE;
        pins.ws_io_num = PIN_MIC_CLK;
        pins.data_out_num = I2S_PIN_NO_CHANGE;
        pins.data_in_num = PIN_MIC_DATA;

        if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK || i2s_set_pin(PORT, &pins) != ESP_OK)
        {
            Serial.println("[mic] I2S setup failed -- mic off");
            return;
        }
        i2s_stop(PORT); // silent until something asks to listen
        installed = true;
        xTaskCreatePinnedToCore(micTask, "mic", 3072, nullptr, 1, nullptr, 0);
    }

    void listenFor(uint32_t ms)
    {
        if (!installed) return;
        uint32_t until = millis() + ms;
        if ((int32_t)(until - listenUntil) > 0) listenUntil = until;
    }

    bool heardVoiceWithin(uint32_t ms)
    {
        uint32_t at = lastVoiceAt;
        return installed && at != 0 && millis() - at <= ms;
    }

    bool available() { return installed; }
}

#else

namespace Mic
{
    void begin() {}
    void listenFor(uint32_t) {}
    bool heardVoiceWithin(uint32_t) { return false; }
    bool available() { return false; }
}

#endif
