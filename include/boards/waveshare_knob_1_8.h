#pragma once

// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// Taken from Waveshare's schematic (ESP32-S3-Knob-Touch-LCD-1.8-schematic,
// sheet 2) and cross-checked against their Arduino demos (08_LVGL_Test,
// 04_Encoder_Test, 03_DRV2605_Test).
//
// The board carries a second MCU (an ESP32-U4WDH) with its own knob, audio
// DAC and USB mux. terraDial runs on the ESP32-S3 only and ignores it.
//
// Included via pins.h -- don't include this directly.

// ---- What this board has (see pins.h for what each one means) ----
#define BOARD_NAME "Waveshare Knob 1.8\""
#define PANEL_RES 360
#define BOARD_PANEL_EVEN_WINDOWS 1 // see roundArea() in main.cpp
#define BOARD_HAS_KNOB_BUTTON 0 // the knob turns but doesn't click
#define BOARD_HAS_LED_RING 0
#define BOARD_HAS_SCREEN_RING 1 // ...so the ring is drawn on the display
#define BOARD_HAS_POWER_RAILS 0
#define BOARD_HAS_HAPTICS 1
#define BOARD_HAS_BATTERY 1
#define BOARD_HAS_MIC 1
#define BOARD_ENCODER_QUADRATURE 0 // pulse-per-direction, see encoder.cpp
#define BOARD_OTA_ASSET_NAME "terradial-knob18-ota.bin"

// ---- Display (ST77916, QSPI on SPI2_HOST) ----
#define PIN_LCD_SCLK 13
#define PIN_LCD_CS 14
#define PIN_LCD_D0 15
#define PIN_LCD_D1 16
#define PIN_LCD_D2 17
#define PIN_LCD_D3 18
#define PIN_LCD_RST 21
// Drives a MOSFET on the backlight's low side: high = lit, PWM dims.
#define PIN_LCD_BACKLIGHT 47

// ---- Touch (CST816, on Wire1) ----
#define PIN_TOUCH_SDA 11
#define PIN_TOUCH_SCL 12
#define PIN_TOUCH_INT 9
#define PIN_TOUCH_RST 10

// ---- Haptics (DRV2605, same I2C bus as touch) ----
// EN is tied to 3V3 and TRIG to GND on the board, so it's I2C-only:
// effects are fired with the GO register.
#define HAPTICS_I2C_ADDR 0x5A

// ---- Knob (the S3's encoder, EC1) ----
// No push switch: PIN_ENCODER_SW is deliberately absent.
#define PIN_ENCODER_A 8
#define PIN_ENCODER_B 7

// ---- Microphone (PDM, straight onto the S3) ----
#define PIN_MIC_CLK 45
#define PIN_MIC_DATA 46

// ---- Battery sense ----
// The LiPo and its charger live on the back board and feed the "5V"
// system rail; GPIO1 sees that rail through a 10k/10k divider (schematic
// sheet 4). So on battery it reads the cell voltage, and on USB it reads
// ~5V whatever the cell is doing -- charging and charged look the same.
#define PIN_BATT_ADC 1
#define BATT_ADC_DIVIDER 2

// ---- On-screen status ring ----
// How many "LEDs" the on-screen ring has (src/led/screen_ring.h).
#define LED_RING_COUNT 24

// Backlight PWM channel config (ledc)
#define BACKLIGHT_PWM_CHANNEL 0
#define BACKLIGHT_PWM_FREQ 5000
#define BACKLIGHT_PWM_RESOLUTION 8

// ---- Physical mounting rotation ----
// LovyanGFX setRotation() step, 0-3. The knob is round, so "up" is wherever
// the USB-C port should sit -- 0 is the panel's native orientation, which
// is a guess until it's seen on the hardware. main.cpp's touch transform
// follows this, so change only this value.
#define DISPLAY_ROTATION 0
