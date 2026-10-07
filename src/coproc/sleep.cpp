// Firmware for the Waveshare Knob's SECOND chip -- not terraDial.
//
// The ESP32-S3-Knob-Touch-LCD-1.8 carries two microcontrollers. terraDial
// runs on the ESP32-S3. The other, an ESP32 (U4WDH), ships with Waveshare's
// factory firmware: a Bluetooth A2DP speaker and HID knob, its radio up
// and discoverable for as long as the board has power -- a steady drain on
// the battery for a feature terraDial has no use for.
//
// This replaces it with nothing at all: at power-on the chip goes straight
// into deep sleep, with no wake-up source, and stays there (a few uA) until
// the next power cycle, when it does the same again. Wi-Fi and Bluetooth
// are never started. The terraDial firmware on the S3 is unaffected.
//
// Build/flash: pio run -e waveshare-knob-1_8-coproc-sleep -t upload, with
// the USB-C plug turned the way that reaches the ESP32 (see the README).
// It stays flashable while asleep: the board's USB-serial bridge resets it
// into the bootloader as usual. Waveshare's firmware can be put back from
// their BIN download -- also in the README.

#include <Arduino.h>
#include <esp_sleep.h>

void setup()
{
    // No wake-up source is enabled, so only a reset or power cycle ends
    // this -- after which setup() comes straight back here.
    esp_deep_sleep_start();
}

void loop() {}
