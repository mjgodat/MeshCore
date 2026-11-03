#pragma once

// ESP32-2432S028 "Cheap Yellow Display": ST7789 320x240 on VSPI, XPT2046
// resistive touch on its own HSPI pins, PWM backlight on GPIO21. Pin and panel
// settings are the ones proven on this board by wichat/lora_term.

#include <helpers/ui/ColorTheme.h>
#include <helpers/ui/LGFXDisplay.h>
#include <Preferences.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX_CYD() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      cfg.pin_int = 36;
      cfg.pin_cs = 33;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

class CYDDisplay : public LGFXDisplay {
  LGFX_CYD disp;

  static constexpr const char* kCalNamespace = "tcal";
  static constexpr const char* kCalKey = "lgfx8";
  static constexpr int kRecalibratePin = 0;  // BOOT button

  // Restore saved XPT2046 calibration, or run LovyanGFX's corner-tap routine
  // on first boot (or when BOOT is held at power-up) and save the result.
  void calibrateTouch() {
    uint16_t cal[8] = {};
    Preferences prefs;
    pinMode(kRecalibratePin, INPUT_PULLUP);
    const bool force = digitalRead(kRecalibratePin) == LOW;
    bool have = false;
    if (prefs.begin(kCalNamespace, false)) {
      have = !force && prefs.getBytes(kCalKey, cal, sizeof(cal)) == sizeof(cal);
      prefs.end();
    }
    if (!have) {
      disp.fillScreen(TFT_BLACK);
      disp.setTextColor(TFT_WHITE, TFT_BLACK);
      disp.setTextDatum(lgfx::middle_center);
      disp.drawString("Touch the arrow in each corner",
                      disp.width() / 2, disp.height() / 2);
      disp.calibrateTouch(cal, TFT_RED, TFT_BLACK, 20);
      if (prefs.begin(kCalNamespace, false)) {
        prefs.putBytes(kCalKey, cal, sizeof(cal));
        prefs.end();
      }
      disp.setTextDatum(lgfx::top_left);
    }
    disp.setTouchCalibrate(cal);
  }

public:
  CYDDisplay() : LGFXDisplay(320, 240, disp) {}

  bool begin() {
    if (!LGFXDisplay::begin()) return false;
    calibrateTouch();
    // The zoomed sprite blit can leave power-up noise at the panel edges.
    disp.fillScreen(mesh::ui::color_theme::background(UIColor::window_bkg));
    return true;
  }
};
