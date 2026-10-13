#pragma once

// LGFX panel definition for whichever board this build is for (pins.h).
// CrowPanel: ported from Elecrow's factory example (RotaryScreen_1_28_new.ino).
// Waveshare Knob: LovyanGFX's ST77916 driver, fed Waveshare's own panel
// init sequence (see Panel_KnobST77916 below).

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "pins.h"

#if defined(BOARD_CROWPANEL_1_28)

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_GC9A01 _panel_instance;
    lgfx::Bus_SPI _bus_instance;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 80000000;
            cfg.freq_read = 20000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = PIN_LCD_SCLK;
            cfg.pin_mosi = PIN_LCD_MOSI;
            cfg.pin_miso = PIN_LCD_MISO;
            cfg.pin_dc = PIN_LCD_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = PIN_LCD_CS;
            cfg.pin_rst = PIN_LCD_RST;
            cfg.pin_busy = -1;
            cfg.memory_width = 240;
            cfg.memory_height = 240;
            cfg.panel_width = 240;
            cfg.panel_height = 240;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

#elif defined(BOARD_WAVESHARE_KNOB_1_8)

// LovyanGFX's Panel_ST77916 ships the init sequence for a different
// ST77916 module (Guition JC3636W518). The controller's the same but the
// glass isn't: that sequence leaves out this panel's gamma tables and its
// whole gate-timing block (0x60-0xBF in bank 0x10), and those are what
// make a given piece of glass display anything sensible. This is the
// sequence Waveshare's own demo sends (08_LVGL_Test/lcd_bsp.c), in
// LovyanGFX's command-list format: cmd, length, data..., with
// CMD_INIT_DELAY on the length meaning "then wait the trailing byte ms".
//
// Two lines from Waveshare's list are left to the driver instead:
// INVON (0x21), which LovyanGFX sends itself from cfg.invert, and MADCTL
// (0x36), which it sends from the rotation and cfg.rgb_order.
class Panel_KnobST77916 : public lgfx::Panel_ST77916
{
protected:
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0xF0, 1, 0x28,
            0xF2, 1, 0x28,
            0x73, 1, 0xF0,
            0x7C, 1, 0xD1,
            0x83, 1, 0xE0,
            0x84, 1, 0x61,
            0xF2, 1, 0x82,
            0xF0, 1, 0x00,
            0xF0, 1, 0x01,
            0xF1, 1, 0x01,
            0xB0, 1, 0x56,
            0xB1, 1, 0x4D,
            0xB2, 1, 0x24,
            0xB4, 1, 0x87,
            0xB5, 1, 0x44,
            0xB6, 1, 0x8B,
            0xB7, 1, 0x40,
            0xB8, 1, 0x86,
            0xBA, 1, 0x00,
            0xBB, 1, 0x08,
            0xBC, 1, 0x08,
            0xBD, 1, 0x00,
            0xC0, 1, 0x80,
            0xC1, 1, 0x10,
            0xC2, 1, 0x37,
            0xC3, 1, 0x80,
            0xC4, 1, 0x10,
            0xC5, 1, 0x37,
            0xC6, 1, 0xA9,
            0xC7, 1, 0x41,
            0xC8, 1, 0x01,
            0xC9, 1, 0xA9,
            0xCA, 1, 0x41,
            0xCB, 1, 0x01,
            0xD0, 1, 0x91,
            0xD1, 1, 0x68,
            0xD2, 1, 0x68,
            0xF5, 2, 0x00, 0xA5,
            0xDD, 1, 0x4F,
            0xDE, 1, 0x4F,
            0xF1, 1, 0x10,
            0xF0, 1, 0x00,
            0xF0, 1, 0x02,
            0xE0, 14, 0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34,
            0xE1, 14, 0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33,
            0xF0, 1, 0x10,
            0xF3, 1, 0x10,
            0xE0, 1, 0x07,
            0xE1, 1, 0x00,
            0xE2, 1, 0x00,
            0xE3, 1, 0x00,
            0xE4, 1, 0xE0,
            0xE5, 1, 0x06,
            0xE6, 1, 0x21,
            0xE7, 1, 0x01,
            0xE8, 1, 0x05,
            0xE9, 1, 0x02,
            0xEA, 1, 0xDA,
            0xEB, 1, 0x00,
            0xEC, 1, 0x00,
            0xED, 1, 0x0F,
            0xEE, 1, 0x00,
            0xEF, 1, 0x00,
            0xF8, 1, 0x00,
            0xF9, 1, 0x00,
            0xFA, 1, 0x00,
            0xFB, 1, 0x00,
            0xFC, 1, 0x00,
            0xFD, 1, 0x00,
            0xFE, 1, 0x00,
            0xFF, 1, 0x00,
            0x60, 1, 0x40,
            0x61, 1, 0x04,
            0x62, 1, 0x00,
            0x63, 1, 0x42,
            0x64, 1, 0xD9,
            0x65, 1, 0x00,
            0x66, 1, 0x00,
            0x67, 1, 0x00,
            0x68, 1, 0x00,
            0x69, 1, 0x00,
            0x6A, 1, 0x00,
            0x6B, 1, 0x00,
            0x70, 1, 0x40,
            0x71, 1, 0x03,
            0x72, 1, 0x00,
            0x73, 1, 0x42,
            0x74, 1, 0xD8,
            0x75, 1, 0x00,
            0x76, 1, 0x00,
            0x77, 1, 0x00,
            0x78, 1, 0x00,
            0x79, 1, 0x00,
            0x7A, 1, 0x00,
            0x7B, 1, 0x00,
            0x80, 1, 0x48,
            0x81, 1, 0x00,
            0x82, 1, 0x06,
            0x83, 1, 0x02,
            0x84, 1, 0xD6,
            0x85, 1, 0x04,
            0x86, 1, 0x00,
            0x87, 1, 0x00,
            0x88, 1, 0x48,
            0x89, 1, 0x00,
            0x8A, 1, 0x08,
            0x8B, 1, 0x02,
            0x8C, 1, 0xD8,
            0x8D, 1, 0x04,
            0x8E, 1, 0x00,
            0x8F, 1, 0x00,
            0x90, 1, 0x48,
            0x91, 1, 0x00,
            0x92, 1, 0x0A,
            0x93, 1, 0x02,
            0x94, 1, 0xDA,
            0x95, 1, 0x04,
            0x96, 1, 0x00,
            0x97, 1, 0x00,
            0x98, 1, 0x48,
            0x99, 1, 0x00,
            0x9A, 1, 0x0C,
            0x9B, 1, 0x02,
            0x9C, 1, 0xDC,
            0x9D, 1, 0x04,
            0x9E, 1, 0x00,
            0x9F, 1, 0x00,
            0xA0, 1, 0x48,
            0xA1, 1, 0x00,
            0xA2, 1, 0x05,
            0xA3, 1, 0x02,
            0xA4, 1, 0xD5,
            0xA5, 1, 0x04,
            0xA6, 1, 0x00,
            0xA7, 1, 0x00,
            0xA8, 1, 0x48,
            0xA9, 1, 0x00,
            0xAA, 1, 0x07,
            0xAB, 1, 0x02,
            0xAC, 1, 0xD7,
            0xAD, 1, 0x04,
            0xAE, 1, 0x00,
            0xAF, 1, 0x00,
            0xB0, 1, 0x48,
            0xB1, 1, 0x00,
            0xB2, 1, 0x09,
            0xB3, 1, 0x02,
            0xB4, 1, 0xD9,
            0xB5, 1, 0x04,
            0xB6, 1, 0x00,
            0xB7, 1, 0x00,
            0xB8, 1, 0x48,
            0xB9, 1, 0x00,
            0xBA, 1, 0x0B,
            0xBB, 1, 0x02,
            0xBC, 1, 0xDB,
            0xBD, 1, 0x04,
            0xBE, 1, 0x00,
            0xBF, 1, 0x00,
            0xC0, 1, 0x10,
            0xC1, 1, 0x47,
            0xC2, 1, 0x56,
            0xC3, 1, 0x65,
            0xC4, 1, 0x74,
            0xC5, 1, 0x88,
            0xC6, 1, 0x99,
            0xC7, 1, 0x01,
            0xC8, 1, 0xBB,
            0xC9, 1, 0xAA,
            0xD0, 1, 0x10,
            0xD1, 1, 0x47,
            0xD2, 1, 0x56,
            0xD3, 1, 0x65,
            0xD4, 1, 0x74,
            0xD5, 1, 0x88,
            0xD6, 1, 0x99,
            0xD7, 1, 0x01,
            0xD8, 1, 0xBB,
            0xD9, 1, 0xAA,
            0xF3, 1, 0x01,
            0xF0, 1, 0x00,
            0x11, 1 + lgfx::Panel_Device::CMD_INIT_DELAY, 0x00, 120,
            0x29, 1, 0x00,
            0xFF, 0xFF, // end of table
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class LGFX : public lgfx::LGFX_Device
{
    Panel_KnobST77916 _panel_instance;
    lgfx::Bus_SPI _bus_instance;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            // 40MHz is what LovyanGFX's own ST77916 board config runs at;
            // Waveshare's demo uses esp_lcd's default, which is in the
            // same range. Worth trying higher only once it's proven stable.
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = PIN_LCD_SCLK;
            cfg.pin_io0 = PIN_LCD_D0;
            cfg.pin_io1 = PIN_LCD_D1;
            cfg.pin_io2 = PIN_LCD_D2;
            cfg.pin_io3 = PIN_LCD_D3;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = PIN_LCD_CS;
            cfg.pin_rst = PIN_LCD_RST;
            cfg.pin_busy = -1;
            cfg.memory_width = 360;
            cfg.memory_height = 360;
            cfg.panel_width = 360;
            cfg.panel_height = 360;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false; // the QSPI read path isn't implemented
            // Waveshare's sequence sends INVON, and their demo shows right
            // colours with MADCTL 0x00 (RGB order). If the colours come out
            // negative, flip invert; if red and blue swap, flip rgb_order.
            cfg.invert = true;
            cfg.rgb_order = true;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

#endif

inline void backlightInit()
{
    ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RESOLUTION);
    ledcAttachPin(PIN_LCD_BACKLIGHT, BACKLIGHT_PWM_CHANNEL);
    ledcWrite(BACKLIGHT_PWM_CHANNEL, 255); // full brightness by default
}

// percent: 0-100
inline void backlightSet(uint8_t percent)
{
    if (percent > 100) percent = 100;
    ledcWrite(BACKLIGHT_PWM_CHANNEL, (percent * 255) / 100);
}
