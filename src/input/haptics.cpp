#include "haptics.h"
#include "pins.h"

#if BOARD_HAS_HAPTICS

#include <Arduino.h>
#include <Wire.h>

namespace
{
    // DRV2605 registers (TI datasheet SLOS854, section 8.6).
    const uint8_t REG_STATUS = 0x00;
    const uint8_t REG_MODE = 0x01;
    const uint8_t REG_LIBRARY = 0x03;
    const uint8_t REG_WAVESEQ1 = 0x04;
    const uint8_t REG_WAVESEQ2 = 0x05;
    const uint8_t REG_GO = 0x0C;
    const uint8_t REG_FEEDBACK = 0x1A;
    const uint8_t REG_CONTROL3 = 0x1D;

    // Waveshare's demo drives this motor as an ERM (library 1, the
    // datasheet's "ERM library A"), open loop, so that's what's copied
    // here. Their schematic labels the motor pads LRA_P/LRA_N, though: if
    // the ticks feel mushy or barely there, the motor is probably an LRA,
    // which wants FEEDBACK bit 7 set, library 6 and an auto-calibration
    // run instead.
    const uint8_t LIBRARY_ERM_A = 1;

    // Effect 24, "Sharp Tick 1 - 100%": the shortest crisp effect in the
    // ERM library, so a fast spin still reads as separate ticks rather
    // than one long buzz. Stronger options if it's too faint: 1 (Strong
    // Click 100%) or 4 (Sharp Click 100%).
    const uint8_t EFFECT_DETENT = 24;

    bool present = false;

    bool writeReg(uint8_t reg, uint8_t value)
    {
        Wire1.beginTransmission(HAPTICS_I2C_ADDR);
        Wire1.write(reg);
        Wire1.write(value);
        return Wire1.endTransmission() == 0;
    }

    bool readReg(uint8_t reg, uint8_t &value)
    {
        Wire1.beginTransmission(HAPTICS_I2C_ADDR);
        Wire1.write(reg);
        if (Wire1.endTransmission(false) != 0) return false;
        if (Wire1.requestFrom((uint8_t)HAPTICS_I2C_ADDR, (uint8_t)1) != 1) return false;
        value = Wire1.read();
        return true;
    }
}

namespace Haptics
{
    void begin()
    {
        uint8_t status;
        if (!readReg(REG_STATUS, status))
        {
            Serial.println("[haptics] no DRV2605 answering -- haptics off");
            return;
        }

        uint8_t feedback = 0, control3 = 0;
        readReg(REG_FEEDBACK, feedback);
        readReg(REG_CONTROL3, control3);

        present = writeReg(REG_MODE, 0x00) // out of standby, internal trigger
                  && writeReg(REG_FEEDBACK, feedback & 0x7F) // bit 7 clear: ERM
                  && writeReg(REG_CONTROL3, control3 | 0x20) // ERM open loop
                  && writeReg(REG_LIBRARY, LIBRARY_ERM_A)
                  // Load the detent effect once; detent() then only has to
                  // press GO. WAVESEQ2 = 0 ends the sequence after it.
                  && writeReg(REG_WAVESEQ1, EFFECT_DETENT)
                  && writeReg(REG_WAVESEQ2, 0);

        Serial.printf("[haptics] DRV2605 (id %u) %s\n", status >> 5, present ? "ready" : "failed to configure");
    }

    void detent()
    {
        if (present) writeReg(REG_GO, 1);
    }
}

#else

namespace Haptics
{
    void begin() {}
    void detent() {}
}

#endif
