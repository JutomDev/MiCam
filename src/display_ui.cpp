#include "display_ui.h"
#include "buzzer.h"
#include <TJpg_Decoder.h>
#include "esp_log.h"

static const char* TAG = "DISPLAY_UI";

LGFX_ST7789 DisplayUI::_lcd;
uint8_t DisplayUI::_brightness = 255;  // Default brightness: 100%
bool DisplayUI::_hudDirty = true;

// Cached HUD values so we only redraw the bottom bar when state changes (zero flickering!)
static char _lastPreset[24] = "";
static uint16_t _lastPhotoCount = 0xFFFF;
static bool _lastSdOk = false;

// TJpg_Decoder output callback rendering decoded MCU blocks onto ST7789
static bool tjpg_output_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= DisplayUI::getLcd().height() || x >= DisplayUI::getLcd().width()) return true;
    DisplayUI::getLcd().pushImage(x, y, w, h, bitmap);
    // Periodically feed the task watchdog during large JPEG decodes
    if ((y & 0x1F) == 0) {
        yield();
    }
    return true;
}

bool DisplayUI::init() {
    ESP_LOGI(TAG, "Initializing ST7789 display with LovyanGFX on SPI3...");
    _lcd.init();
    _lcd.setRotation(0);
    _lcd.setColorDepth(16);
    _lcd.fillScreen(TFT_BLACK);

    // TJpgDec decoder setup for rendering images from SD card
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tjpg_output_cb);

    setBrightness(_brightness);

    // Retro Boot Splash animation: MIphoto
    _lcd.fillScreen(0x0821);
    _lcd.drawRect(8, 8, TFT_WIDTH - 16, TFT_HEIGHT - 16, 0xFD20);
    _lcd.drawRect(12, 12, TFT_WIDTH - 24, TFT_HEIGHT - 24, 0x31A6);

    _lcd.setTextSize(3);
    _lcd.setTextColor(0xFD20, 0x0821);  // Retro Amber
    _lcd.drawCenterString("MIphoto", 120, 75);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0x07FF, 0x0821);  // Cyan
    _lcd.drawCenterString("RETRO COMPACT CAMERA", 120, 118);

    // Cute retro cat icon in ASCII/text art
    _lcd.setTextColor(0xFFFF, 0x0821);
    _lcd.drawCenterString("/\\_/\\   \"Mii!\"", 120, 150);
    _lcd.drawCenterString("( o.o )        ", 120, 164);
    _lcd.drawCenterString(" > ^ <         ", 120, 178);

    _lcd.setTextColor(0x07E0, 0x0821);  // Green
    _lcd.drawCenterString("SYSTEM READY", 120, 215);

    _hudDirty = true;
    return true;
}

LGFX_ST7789& DisplayUI::getLcd() {
    return _lcd;
}

void DisplayUI::invalidateHud() {
    _hudDirty = true;
}

void DisplayUI::setBrightness(uint8_t brightness) {
    _brightness = brightness;
    _lcd.setBrightness(_brightness);
    _hudDirty = true;
}

uint8_t DisplayUI::getBrightness() {
    return _brightness;
}

void DisplayUI::cycleBrightness() {
    if (_brightness >= 240)
        _brightness = 64;  // 25%
    else if (_brightness <= 64)
        _brightness = 128;  // 50%
    else if (_brightness <= 128)
        _brightness = 192;  // 75%
    else
        _brightness = 255;  // 100%
    setBrightness(_brightness);
}

// Helper to write a big-endian RGB565 pixel directly into the camera framebuffer in RAM
static inline void putPxBE(uint16_t* buf, uint16_t w, uint16_t h, int x, int y, uint16_t colorLE) {
    if (x >= 0 && x < (int)w && y >= 0 && y < (int)h) {
        buf[y * w + x] = (colorLE >> 8) | (colorLE << 8);
    }
}

void DisplayUI::drawReticleInBuffer(uint16_t* buf, uint16_t w, uint16_t h) {
    if (!buf) return;
    // Retro Amber (0xFD20) drawn directly into RAM before DMA push -> 0% tearing / 0% flicker!
    const uint16_t reticleColor = 0xFD20;
    const int cornerLen = 16;
    const int pad = 20;
    const int right = (int)w - pad - 1;
    const int bottom = (int)h - pad - 1;

    for (int i = 0; i < cornerLen; i++) {
        // Top-left
        putPxBE(buf, w, h, pad + i, pad, reticleColor);
        putPxBE(buf, w, h, pad, pad + i, reticleColor);
        // Top-right
        putPxBE(buf, w, h, right - i, pad, reticleColor);
        putPxBE(buf, w, h, right, pad + i, reticleColor);
        // Bottom-left
        putPxBE(buf, w, h, pad + i, bottom, reticleColor);
        putPxBE(buf, w, h, pad, bottom - i, reticleColor);
        // Bottom-right
        putPxBE(buf, w, h, right - i, bottom, reticleColor);
        putPxBE(buf, w, h, right, bottom - i, reticleColor);
    }

    // Center crosshair
    int cx = w / 2;
    int cy = h / 2;
    for (int d = -5; d <= 5; d++) {
        putPxBE(buf, w, h, cx + d, cy, reticleColor);
        putPxBE(buf, w, h, cx, cy + d, reticleColor);
    }
}

void DisplayUI::drawHud(const char* presetName, uint16_t photoCount, bool sdOk) {
    // Only redraw the HUD bar if something actually changed!
    if (!_hudDirty && strncmp(_lastPreset, presetName ? presetName : "", sizeof(_lastPreset) - 1) == 0 &&
        _lastPhotoCount == photoCount && _lastSdOk == sdOk) {
        return;
    }

    _hudDirty = false;
    strncpy(_lastPreset, presetName ? presetName : "", sizeof(_lastPreset) - 1);
    _lastPreset[sizeof(_lastPreset) - 1] = '\0';
    _lastPhotoCount = photoCount;
    _lastSdOk = sdOk;

    if (TFT_HEIGHT > 240) {
        // 1.69" display (240x280): Dedicated bottom telemetry bar (y: 240..279)
        uint16_t hudY = 240;
        _lcd.startWrite();
        _lcd.fillRect(0, hudY, TFT_WIDTH, TFT_HEIGHT - hudY, 0x0000);
        _lcd.drawFastHLine(0, hudY, TFT_WIDTH, 0xFD20);  // Gold dividing line

        // Preset Badge
        _lcd.fillRect(4, hudY + 7, 92, 26, 0x2104);
        _lcd.drawRect(4, hudY + 7, 92, 26, 0xFD20);
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFD20, 0x2104);
        _lcd.drawString(presetName, 10, hudY + 15);

        // SD Card Status Icon
        if (sdOk) {
            _lcd.setTextColor(0x07E0, 0x0000);
            _lcd.drawString("SD:OK", 104, hudY + 15);
        } else {
            _lcd.setTextColor(0xF800, 0x0000);
            _lcd.drawString("NO SD", 104, hudY + 15);
        }

        // Photo Counter
        char countStr[16];
        snprintf(countStr, sizeof(countStr), "[#%04d]", photoCount);
        _lcd.setTextColor(0x07FF, 0x0000);
        _lcd.drawRightString(countStr, TFT_WIDTH - 6, hudY + 15);
        _lcd.endWrite();
    } else {
        _lcd.fillRect(0, 0, TFT_WIDTH, 20, 0x0000);
        _lcd.setTextSize(1);
        _lcd.fillRect(4, 3, 76, 14, 0x2104);
        _lcd.drawRect(4, 3, 76, 14, 0xFD20);
        _lcd.setTextColor(0xFD20, 0x2104);
        _lcd.drawString(presetName, 8, 6);

        if (sdOk) {
            _lcd.setTextColor(0x07E0, 0x0000);
            _lcd.drawRightString("SD OK", TFT_WIDTH - 6, 6);
        } else {
            _lcd.setTextColor(0xF800, 0x0000);
            _lcd.drawRightString("NO SD", TFT_WIDTH - 6, 6);
        }
    }
}

void DisplayUI::drawLiveView(uint8_t* rgb565_buf, uint16_t w, uint16_t h, const char* presetName, uint16_t photoCount,
                             bool sdOk) {
    if (!rgb565_buf) return;

    uint16_t* pxBuf = reinterpret_cast<uint16_t*>(rgb565_buf);
    size_t count = (size_t)w * h;

    // Software brightness control when hardware BLK pin is tied to 3.3V (-1)
    if (_brightness < 240) {
        uint8_t shift = (_brightness <= 64) ? 2 : 1;
        for (size_t i = 0; i < count; i++) {
            uint16_t px = pxBuf[i];
            uint16_t v = (px >> 8) | (px << 8);
            uint16_t r = ((v >> 11) & 0x1F) >> shift;
            uint16_t g = ((v >> 5) & 0x3F) >> shift;
            uint16_t b = (v & 0x1F) >> shift;
            if (_brightness > 128 && shift == 1) {
                // 75% brightness: add 1/4 back
                r += ((v >> 11) & 0x1F) >> 2;
                g += ((v >> 5) & 0x3F) >> 2;
                b += (v & 0x1F) >> 2;
            }
            uint16_t out = (r << 11) | (g << 5) | b;
            pxBuf[i] = (out >> 8) | (out << 8);
        }
    }

    // Draw viewfinder reticle directly into RAM buffer before pushing -> zero flicker!
    drawReticleInBuffer(pxBuf, w, h);

    // Push 240x240 RGB565 camera framebuffer in a single atomic DMA transfer
    _lcd.pushImage(0, 0, w, h, reinterpret_cast<const lgfx::swap565_t*>(rgb565_buf));

    // Only redraw bottom HUD bar if state changed
    drawHud(presetName, photoCount, sdOk);
}

void DisplayUI::drawFlashAnimation() {
    // High-speed white screen flash while keeping buzzer melody alive
    _lcd.fillRect(0, 0, TFT_WIDTH, 240, 0xFFFF);
    Buzzer::delayMs(40);
    _lcd.fillRect(0, 0, TFT_WIDTH, 240, 0x0000);
    _hudDirty = true;
}

void DisplayUI::drawSavingProgress(const char* filename) {
    _lcd.fillRect(36, 95, 168, 48, 0x10A2);
    _lcd.drawRect(36, 95, 168, 48, 0xFD20);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFFFF, 0x10A2);
    _lcd.drawCenterString("SAVING PHOTO...", 120, 105);
    _lcd.setTextColor(0x07FF, 0x10A2);
    _lcd.drawCenterString(filename ? filename : "PROCESSING", 120, 124);
    Buzzer::update();
    _hudDirty = true;
}

void DisplayUI::drawGalleryPhoto(const char* filepath, uint16_t currentIdx, uint16_t totalCount) {
    Buzzer::stop();  // Silence any pending navigation tone immediately
    _hudDirty = true;

    if (!filepath || strlen(filepath) == 0) {
        drawGalleryEmpty();
        return;
    }

    // 1. Verify file exists on SD card
    if (!SD.exists(filepath)) {
        ESP_LOGW(TAG, "Gallery file missing: %s", filepath);
        _lcd.fillScreen(0x0821);
        _lcd.drawRect(16, 45, TFT_WIDTH - 32, 175, 0xF800);
        _lcd.setTextSize(2);
        _lcd.setTextColor(0xF800, 0x0821);
        _lcd.drawCenterString("FILE MISSING", 120, 75);
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFFFF, 0x0821);
        _lcd.drawCenterString(filepath, 120, 110);
        _lcd.setTextColor(0xFD20, 0x0821);
        _lcd.drawCenterString("File not found on SD card", 120, 132);
        _lcd.setTextColor(0x07FF, 0x0821);
        _lcd.drawCenterString("< / > Skip | Hold D0: Del", 120, 165);

        // Header and Footer
        _lcd.fillRect(0, 0, TFT_WIDTH, 26, 0x18C3);
        _lcd.drawFastHLine(0, 26, TFT_WIDTH, 0xFD20);
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFD20, 0x18C3);
        _lcd.drawString("GALLERY (< / >)", 8, 9);
        char idxStr[24];
        snprintf(idxStr, sizeof(idxStr), "%d / %d", currentIdx, totalCount);
        _lcd.setTextColor(0xFFFF, 0x18C3);
        _lcd.drawRightString(idxStr, TFT_WIDTH - 8, 9);

        _lcd.fillRect(0, TFT_HEIGHT - 26, TFT_WIDTH, 26, 0x18C3);
        _lcd.drawFastHLine(0, TFT_HEIGHT - 26, TFT_WIDTH, 0x31A6);
        _lcd.setTextColor(0x07FF, 0x18C3);
        _lcd.drawCenterString(filepath, 120, TFT_HEIGHT - 17);
        return;
    }

    // 2. Inspect JPEG dimensions safely without crashing
    uint16_t jpgW = 0, jpgH = 0;
    JRESULT sizeRes = TJpgDec.getSdJpgSize(&jpgW, &jpgH, filepath);
    if (sizeRes != JDR_OK || jpgW == 0 || jpgH == 0) {
        ESP_LOGW(TAG, "Corrupt JPEG in gallery: %s (err %d)", filepath, (int)sizeRes);
        _lcd.fillScreen(0x0821);
        _lcd.drawRect(16, 45, TFT_WIDTH - 32, 175, 0xF800);
        _lcd.setTextSize(2);
        _lcd.setTextColor(0xF800, 0x0821);
        _lcd.drawCenterString("CORRUPT PHOTO", 120, 75);
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFFFF, 0x0821);
        _lcd.drawCenterString(filepath, 120, 110);
        _lcd.setTextColor(0xFD20, 0x0821);
        _lcd.drawCenterString("Damaged file on SD card", 120, 132);
        _lcd.setTextColor(0x07FF, 0x0821);
        _lcd.drawCenterString("< / > Skip | Hold D0: Del", 120, 165);

        // Header and Footer
        _lcd.fillRect(0, 0, TFT_WIDTH, 26, 0x18C3);
        _lcd.drawFastHLine(0, 26, TFT_WIDTH, 0xFD20);
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFD20, 0x18C3);
        _lcd.drawString("GALLERY (< / >)", 8, 9);
        char idxStr[24];
        snprintf(idxStr, sizeof(idxStr), "%d / %d", currentIdx, totalCount);
        _lcd.setTextColor(0xFFFF, 0x18C3);
        _lcd.drawRightString(idxStr, TFT_WIDTH - 8, 9);

        _lcd.fillRect(0, TFT_HEIGHT - 26, TFT_WIDTH, 26, 0x18C3);
        _lcd.drawFastHLine(0, TFT_HEIGHT - 26, TFT_WIDTH, 0x31A6);
        _lcd.setTextColor(0x07FF, 0x18C3);
        _lcd.drawCenterString(filepath, 120, TFT_HEIGHT - 17);
        return;
    }

    _lcd.fillScreen(0x0000);

    // 3. Compute best scaling factor (1, 2, 4, 8) to fit in 240x224
    uint8_t scale = 1;
    if (jpgW > 800 || jpgH > 800) {
        scale = 8;
    } else if (jpgW > 400 || jpgH > 400) {
        scale = 4;
    } else if (jpgW > 240 || jpgH > 224) {
        scale = 2;
    } else {
        scale = 1;
    }

    uint16_t drawW = jpgW / scale;
    uint16_t drawH = jpgH / scale;
    TJpgDec.setJpgScale(scale);

    int16_t posX = (TFT_WIDTH > drawW) ? (TFT_WIDTH - drawW) / 2 : 0;
    int16_t posY = 26 + ((224 > drawH) ? (224 - drawH) / 2 : 0);

    // Lock SPI transaction during rendering for maximum speed and safety
    _lcd.startWrite();
    TJpgDec.drawSdJpg(posX, posY, filepath);
    _lcd.endWrite();

    if (drawW <= 236 && drawH <= 220) {
        _lcd.drawRect(posX - 1, posY - 1, drawW + 2, drawH + 2, 0xFD20);
    }

    // Top Header
    _lcd.fillRect(0, 0, TFT_WIDTH, 26, 0x18C3);
    _lcd.drawFastHLine(0, 26, TFT_WIDTH, 0xFD20);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFD20, 0x18C3);
    _lcd.drawString("GALLERY (< / >)", 8, 9);

    char idxStr[24];
    snprintf(idxStr, sizeof(idxStr), "%d / %d", currentIdx, totalCount);
    _lcd.setTextColor(0xFFFF, 0x18C3);
    _lcd.drawRightString(idxStr, TFT_WIDTH - 8, 9);

    // Bottom Footer - filename + controls hint
    _lcd.fillRect(0, TFT_HEIGHT - 26, TFT_WIDTH, 26, 0x18C3);
    _lcd.drawFastHLine(0, TFT_HEIGHT - 26, TFT_WIDTH, 0x31A6);
    _lcd.setTextColor(0x07FF, 0x18C3);
    _lcd.drawCenterString(filepath, 120, TFT_HEIGHT - 17);
}

void DisplayUI::drawGalleryEmpty() {
    _lcd.fillScreen(0x0821);
    _hudDirty = true;
    _lcd.drawRect(16, 60, TFT_WIDTH - 32, 150, 0xFD20);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0xFD20, 0x0821);
    _lcd.drawCenterString("NO PHOTOS", 120, 88);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFFFF, 0x0821);
    _lcd.drawCenterString("Take a photo first with D0!", 120, 125);
    _lcd.setTextColor(0x07FF, 0x0821);
    _lcd.drawCenterString("[ Press SHUTTER / LEFT to exit ]", 120, 165);
}

void DisplayUI::drawMenu(uint8_t selectedIndex, const char* currentPresetName, uint8_t brightnessLevel,
                         bool buzzerMuted, uint16_t photoCount, bool fullRedraw) {
    _hudDirty = true;

    if (fullRedraw) {
        _lcd.fillScreen(0x0821);

        // Menu Header (28 px)
        _lcd.fillRect(0, 0, TFT_WIDTH, 28, 0x0000);
        _lcd.drawFastHLine(0, 28, TFT_WIDTH, 0xFD20);
        _lcd.setTextSize(2);
        _lcd.setTextColor(0xFD20, 0x0000);
        _lcd.drawCenterString("MIPHOTO MENU", 120, 6);
    }

    const char* items[] = {"1. Image Preset",   "2. Screen Brightness",    "3. Speaker Sound", "4. Photo Gallery",
                           "5. Wi-Fi Transfer", "6. Mini-Game: Mii-Snake", "7. Back to Camera"};

    char subText[36];
    _lcd.startWrite();
    for (uint8_t i = 0; i < 7; i++) {
        uint16_t y = 32 + (i * 35);
        bool isSel = (i == selectedIndex);
        uint16_t bgCol = isSel ? 0x2104 : 0x10A2;
        uint16_t borderCol = isSel ? 0xFD20 : 0x31A6;

        _lcd.fillRect(8, y, TFT_WIDTH - 16, 32, bgCol);
        _lcd.drawRect(8, y, TFT_WIDTH - 16, 32, borderCol);
        if (isSel) {
            _lcd.fillRect(10, y + 2, 4, 28, 0xFD20);  // Selection indicator bar
        }

        _lcd.setTextSize(1);
        _lcd.setTextColor(isSel ? 0xFD20 : 0xFFFF, bgCol);
        _lcd.drawString(items[i], 20, y + 5);

        // Subtitle / option detail
        if (i == 0) {
            snprintf(subText, sizeof(subText), "< %s >", currentPresetName);
            _lcd.setTextColor(isSel ? 0x07FF : 0xAD55, bgCol);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 1) {
            int pct = (brightnessLevel * 100 + 127) / 255;
            snprintf(subText, sizeof(subText), "< %d%% >", pct);
            _lcd.setTextColor(isSel ? 0x07FF : 0xAD55, bgCol);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 2) {
            snprintf(subText, sizeof(subText), "< %s >", buzzerMuted ? "MUTED" : "ON");
            _lcd.setTextColor(buzzerMuted ? (isSel ? 0xF800 : 0xAD55) : (isSel ? 0x07E0 : 0xAD55), bgCol);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 3) {
            snprintf(subText, sizeof(subText), "View SD Card (%d photos)", photoCount);
            _lcd.setTextColor(isSel ? 0x07E0 : 0x7BEF, bgCol);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 4) {
            _lcd.setTextColor(isSel ? 0x07E0 : 0x7BEF, bgCol);
            _lcd.drawString("Web App & Live Stream", 20, y + 18);
        } else if (i == 5) {
            _lcd.setTextColor(isSel ? 0xF81F : 0x7BEF, bgCol);
            _lcd.drawString("Play Retro Cat Snake!", 20, y + 18);
        } else if (i == 6) {
            _lcd.setTextColor(isSel ? 0x07FF : 0x7BEF, bgCol);
            _lcd.drawString("Return to viewfinder", 20, y + 18);
        }
    }
    _lcd.endWrite();
}

void DisplayUI::drawWifiServerScreen(const char* ssid, const char* ip, uint16_t photoCount) {
    _lcd.fillScreen(0x0000);
    _hudDirty = true;

    // Wi-Fi Header
    _lcd.fillRect(0, 0, TFT_WIDTH, 34, 0x0010);
    _lcd.drawFastHLine(0, 34, TFT_WIDTH, 0x07E0);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0x07E0, 0x0010);
    _lcd.drawCenterString("WI-FI SHARE", 120, 8);

    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("1. CONNECT TO WI-FI:", 120, 48);

    // SSID and Password box
    _lcd.fillRect(16, 64, TFT_WIDTH - 32, 54, 0x18E3);
    _lcd.drawRect(16, 64, TFT_WIDTH - 32, 54, 0x07FF);

    _lcd.setTextColor(0xFFFF, 0x18E3);
    _lcd.drawString("SSID: ", 26, 74);
    _lcd.setTextColor(0x07FF, 0x18E3);
    _lcd.drawString(ssid, 68, 74);

    _lcd.setTextColor(0xFFFF, 0x18E3);
    _lcd.drawString("PASS: ", 26, 94);
    _lcd.setTextColor(0x07E0, 0x18E3);
    _lcd.drawString(strlen(WIFI_AP_PASS) > 0 ? WIFI_AP_PASS : "OPEN (NO PASS)", 68, 94);

    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("2. OPEN IN BROWSER:", 120, 130);

    // URL / IP box
    _lcd.fillRect(16, 144, TFT_WIDTH - 32, 44, 0x2104);
    _lcd.drawRect(16, 144, TFT_WIDTH - 32, 44, 0x07E0);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0x07E0, 0x2104);
    _lcd.drawCenterString("http://192.168.4.1", 120, 152);
    _lcd.setTextColor(0x07FF, 0x2104);
    _lcd.drawCenterString("http://miphoto.local", 120, 170);

    char infoStr[32];
    snprintf(infoStr, sizeof(infoStr), "Photos on SD: %d", photoCount);
    _lcd.setTextColor(0xFFFF, 0x0000);
    _lcd.drawCenterString(infoStr, 120, 206);

    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("[ Press SHUTTER / LEFT to exit ]", 120, 240);
}

void DisplayUI::showMessage(const char* title, const char* subtitle, uint16_t delayMs) {
    _hudDirty = true;
    _lcd.fillRect(20, 80, TFT_WIDTH - 40, 85, 0x0821);
    _lcd.drawRect(20, 80, TFT_WIDTH - 40, 85, 0xFD20);
    _lcd.drawRect(22, 82, TFT_WIDTH - 44, 81, 0x31A6);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0xFD20, 0x0821);
    _lcd.drawCenterString(title, 120, 96);
    if (subtitle) {
        _lcd.setTextSize(1);
        _lcd.setTextColor(0x07FF, 0x0821);
        _lcd.drawCenterString(subtitle, 120, 132);
    }
    if (delayMs > 0) {
        Buzzer::delayMs(delayMs);
    }
}
