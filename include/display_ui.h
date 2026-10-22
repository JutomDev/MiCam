#pragma once

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <SD.h>
#include "config.h"

// ============================================================================
// Dedicated hardware LovyanGFX configuration for ST7789 on SPI3_HOST
// ============================================================================
class LGFX_ST7789 : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus_instance;
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX_ST7789() {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI3_HOST;  // Fully isolated from MicroSD bus (SPI2_HOST)
            cfg.spi_mode = 0;
            cfg.freq_write = TFT_SPI_FREQ;  // 40 MHz
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = TFT_SCLK_PIN;
            cfg.pin_mosi = TFT_MOSI_PIN;
            cfg.pin_miso = -1;
            cfg.pin_dc = TFT_DC_PIN;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = TFT_CS_PIN;
            cfg.pin_rst = TFT_RST_PIN;
            cfg.pin_busy = -1;
            cfg.panel_width = TFT_WIDTH;
            cfg.panel_height = TFT_HEIGHT;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;  // Required for ST7789 IPS panels
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;  // Dedicated SPI bus
            _panel_instance.config(cfg);
        }

#if (TFT_BLK_PIN >= 0)
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = TFT_BLK_PIN;
            cfg.invert = false;
            cfg.freq = 20000;  // 20 kHz PWM
            cfg.pwm_channel = 6;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
#endif

        setPanel(&_panel_instance);
    }
};

class DisplayUI {
public:
    static bool init();
    static void setBrightness(uint8_t brightness);
    static uint8_t getBrightness();
    static void cycleBrightness();

    static void drawLiveView(const uint8_t* rgb565_buf, uint16_t w, uint16_t h, const char* presetName,
                             uint16_t photoCount, bool sdOk);

    static void drawFlashAnimation();
    static void drawSavingProgress(const char* filename);
    static void drawGalleryPhoto(const char* filepath, uint16_t currentIdx, uint16_t totalCount);
    static void drawGalleryEmpty();
    static void drawMenu(uint8_t selectedIndex, const char* currentPresetName, uint8_t brightnessLevel);
    static void drawWifiServerScreen(const char* ssid, const char* ip, uint16_t photoCount);
    static void showMessage(const char* title, const char* subtitle, uint16_t delayMs = 0);

    static LGFX_ST7789& getLcd();

private:
    static LGFX_ST7789 _lcd;
    static uint8_t _brightness;
    static void drawReticle();
    static void drawHud(const char* presetName, uint16_t photoCount, bool sdOk);
};
