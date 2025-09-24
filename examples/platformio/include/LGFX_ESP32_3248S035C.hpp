#pragma once
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  // coba ganti ST7796 kalau ILI9488 gagal
  lgfx::Panel_ILI9488 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM backlight;
  lgfx::Touch_FT5x06 touch;

public:
  LGFX(void) {
    { // SPI bus config
      auto cfg = bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = 1;

      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;

      bus.config(cfg);
      panel.setBus(&bus);
    }

    { // Panel config
      auto cfg = panel.config();
      cfg.pin_cs  = 15;
      cfg.pin_rst = -1;   // auto-reset
      cfg.pin_busy = -1;

      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 4;

      cfg.dummy_read_pixel = 0;
      cfg.dummy_read_bits  = 0;
      cfg.readable = false;
      cfg.invert = false;       // kalau warna kebalik, ganti true
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;

      panel.config(cfg);
    }

    { // Backlight
      auto cfg = backlight.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq   = 44100;
      cfg.pwm_channel = 7;
      backlight.config(cfg);
      panel.setLight(&backlight);
    }

    { // Touch FT6336 (I2C)
      auto cfg = touch.config();
      cfg.i2c_port = 1;        // Arduino pakai angka
      cfg.i2c_addr = 0x38;
      cfg.pin_sda  = 21;
      cfg.pin_scl  = 22;
      cfg.pin_int  = -1;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.bus_shared = false;
      touch.config(cfg);
      panel.setTouch(&touch);
    }

    setPanel(&panel);
  }
};
