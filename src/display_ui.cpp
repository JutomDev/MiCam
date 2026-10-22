#include "display_ui.h"
#include <TJpg_Decoder.h>
#include "esp_log.h"

static const char* TAG = "DISPLAY_UI";

LGFX_ST7789 DisplayUI::_lcd;
uint8_t DisplayUI::_brightness = 200;  // Default brightness (approx. 80%)

// TJpg_Decoder output callback rendering decoded MCU blocks onto ST7789
static bool tjpg_output_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= DisplayUI::getLcd().height()) return false;
    DisplayUI::getLcd().pushImage(x, y, w, h, bitmap);
    return true;
}

bool DisplayUI::init() {
    ESP_LOGI(TAG, "Initializing ST7789 display with LovyanGFX on SPI3...");
    _lcd.init();
    _lcd.setRotation(0);
    _lcd.setColorDepth(16);
    _lcd.fillScreen(TFT_BLACK);

    // TJpgDec decoder setup for rendering images from SD card
    TJpgDec.setJpgScale(8);  // 1/8 scale: 1600x1200 JPEG -> 200x150 thumbnail (fits 240x240 screen)
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tjpg_output_cb);

    setBrightness(_brightness);

    // Retro Boot Splash animation: MIphoto
    _lcd.fillScreen(0x0821);
    _lcd.setTextSize(3);
    _lcd.setTextColor(0xFD20, 0x0821);  // Retro Amber
    _lcd.drawCenterString("MIphoto", 120, 80);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0x07FF, 0x0821);  // Cyan
    _lcd.drawCenterString("COMPACT DIGICAM", 120, 118);
    _lcd.setTextColor(0x07E0, 0x0821);  // Green
    _lcd.drawCenterString("SYSTEM READY", 120, 142);

    delay(750);
    _lcd.fillScreen(TFT_BLACK);
    return true;
}

LGFX_ST7789& DisplayUI::getLcd() {
    return _lcd;
}

void DisplayUI::setBrightness(uint8_t brightness) {
    _brightness = brightness;
    _lcd.setBrightness(_brightness);
}

uint8_t DisplayUI::getBrightness() {
    return _brightness;
}

void DisplayUI::cycleBrightness() {
    if (_brightness <= 64)
        _brightness = 128;
    else if (_brightness <= 128)
        _brightness = 192;
    else if (_brightness <= 192)
        _brightness = 255;
    else
        _brightness = 64;
    setBrightness(_brightness);
}

void DisplayUI::drawReticle() {
    // Viewfinder with retro frame corner markers
    uint16_t reticleColor = 0xFD20;  // Retro Amber
    uint16_t cornerLen = 16;
    uint16_t pad = 24;
    uint16_t w = TFT_WIDTH - pad;
    uint16_t h = TFT_HEIGHT - pad;

    // Top-left
    _lcd.drawFastHLine(pad, pad, cornerLen, reticleColor);
    _lcd.drawFastVLine(pad, pad, cornerLen, reticleColor);

    // Top-right
    _lcd.drawFastHLine(w - cornerLen, pad, cornerLen, reticleColor);
    _lcd.drawFastVLine(w, pad, cornerLen, reticleColor);

    // Bottom-left
    _lcd.drawFastHLine(pad, h, cornerLen, reticleColor);
    _lcd.drawFastVLine(pad, h - cornerLen, cornerLen, reticleColor);

    // Bottom-right
    _lcd.drawFastHLine(w - cornerLen, h, cornerLen, reticleColor);
    _lcd.drawFastVLine(w, h - cornerLen, cornerLen, reticleColor);

    // Center crosshair
    uint16_t cx = TFT_WIDTH / 2;
    uint16_t cy = TFT_HEIGHT / 2;
    _lcd.drawFastHLine(cx - 5, cy, 11, reticleColor);
    _lcd.drawFastVLine(cx, cy - 5, 11, reticleColor);
}

void DisplayUI::drawHud(const char* presetName, uint16_t photoCount, bool sdOk) {
    if (TFT_HEIGHT > 240) {
        // 1.69" display (240x280): Dedicated bottom telemetry bar (y: 240..279)
        // 240x240 camera viewport remains 100% clean and unobstructed
        uint16_t hudY = 240;
        _lcd.fillRect(0, hudY, TFT_WIDTH, TFT_HEIGHT - hudY, 0x0000);
        _lcd.drawFastHLine(0, hudY, TFT_WIDTH, 0xFD20);  // Gold dividing line

        // Preset Badge
        _lcd.fillRect(4, hudY + 7, 88, 26, 0x2104);
        _lcd.drawRect(4, hudY + 7, 88, 26, 0xFD20);
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
    } else {
        // Standard square 240x240 display
        _lcd.fillRect(0, 0, TFT_WIDTH, 20, 0x0000);
        _lcd.setTextSize(1);

        // Preset Badge
        _lcd.fillRect(4, 3, 76, 14, 0x2104);
        _lcd.drawRect(4, 3, 76, 14, 0xFD20);
        _lcd.setTextColor(0xFD20, 0x2104);
        _lcd.drawString(presetName, 8, 6);

        // SD Card Status
        if (sdOk) {
            _lcd.setTextColor(0x07E0, 0x0000);
            _lcd.drawRightString("SD OK", TFT_WIDTH - 6, 6);
        } else {
            _lcd.setTextColor(0xF800, 0x0000);
            _lcd.drawRightString("NO SD", TFT_WIDTH - 6, 6);
        }

        // Bottom Bar
        _lcd.fillRect(0, TFT_HEIGHT - 20, TFT_WIDTH, 20, 0x0000);
        _lcd.setTextColor(0xAD55, 0x0000);
        _lcd.drawString("UXGA 4:3", 6, TFT_HEIGHT - 15);

        char countStr[16];
        snprintf(countStr, sizeof(countStr), "[#%04d]", photoCount);
        _lcd.setTextColor(0x07FF, 0x0000);
        _lcd.drawRightString(countStr, TFT_WIDTH - 6, TFT_HEIGHT - 15);
    }
}

void DisplayUI::drawLiveView(const uint8_t* rgb565_buf, uint16_t w, uint16_t h, const char* presetName,
                             uint16_t photoCount, bool sdOk) {
    if (!rgb565_buf) return;

    // Push 240x240 RGB565 camera framebuffer directly via DMA
    _lcd.pushImage(0, 0, w, h, reinterpret_cast<const uint16_t*>(rgb565_buf));

    // Draw reticle and HUD
    drawReticle();
    drawHud(presetName, photoCount, sdOk);
}

void DisplayUI::drawFlashAnimation() {
    // High-speed white screen flash simulating mechanical flash discharge
    _lcd.fillScreen(0xFFFF);
    delay(45);
    _lcd.fillScreen(0x0000);
}

void DisplayUI::drawSavingProgress(const char* filename) {
    _lcd.fillRect(40, 100, 160, 45, 0x10A2);
    _lcd.drawRect(40, 100, 160, 45, 0xFD20);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFFFF, 0x10A2);
    _lcd.drawCenterString("SAVING TO SD...", 120, 108);
    _lcd.setTextColor(0x07FF, 0x10A2);
    _lcd.drawCenterString(filename ? filename : "PROCESSING", 120, 126);
}

void DisplayUI::drawGalleryPhoto(const char* filepath, uint16_t currentIdx, uint16_t totalCount) {
    _lcd.fillScreen(0x0000);

    // Decode and render JPEG at 1/8 scale centered on display
    // 1600x1200 / 8 = 200x150, centered: x=(240-200)/2=20, y=(240-150)/2=45
    uint16_t posX = 20;
    uint16_t posY = 38;

    TJpgDec.drawSdJpg(posX, posY, filepath);

    // Retro slide photo frame
    _lcd.drawRect(posX - 1, posY - 1, 202, 152, 0x7BEF);

    // Top Header
    _lcd.fillRect(0, 0, TFT_WIDTH, 26, 0x18C3);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFD20, 0x18C3);
    _lcd.drawString("GALLERY", 8, 8);

    char idxStr[24];
    snprintf(idxStr, sizeof(idxStr), "%d / %d", currentIdx, totalCount);
    _lcd.setTextColor(0xFFFF, 0x18C3);
    _lcd.drawRightString(idxStr, TFT_WIDTH - 8, 8);

    // Bottom Footer - filename
    _lcd.fillRect(0, TFT_HEIGHT - 26, TFT_WIDTH, 26, 0x18C3);
    _lcd.setTextColor(0x07FF, 0x18C3);
    _lcd.drawCenterString(filepath, 120, TFT_HEIGHT - 18);
}

void DisplayUI::drawGalleryEmpty() {
    _lcd.fillScreen(0x0000);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("GALLERY IS EMPTY", 120, 100);
    _lcd.setTextColor(0x7BEF, 0x0000);
    _lcd.drawCenterString("Take a photo first!", 120, 125);
    _lcd.drawCenterString("Click NAV to exit", 120, 150);
}

void DisplayUI::drawMenu(uint8_t selectedIndex, const char* currentPresetName, uint8_t brightnessLevel) {
    _lcd.fillScreen(0x0821);

    // Menu Header
    _lcd.fillRect(0, 0, TFT_WIDTH, 32, 0x0000);
    _lcd.drawFastHLine(0, 32, TFT_WIDTH, 0xFD20);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("CAMERA MENU", 120, 7);

    const char* items[] = {"Image Preset", "Screen Brightness", "Wi-Fi Transfer", "Photo Gallery", "Back"};

    char subText[32];
    for (uint8_t i = 0; i < 5; i++) {
        uint16_t y = 46 + (i * 38);
        bool isSel = (i == selectedIndex);

        if (isSel) {
            _lcd.fillRect(8, y, TFT_WIDTH - 16, 32, 0x2104);
            _lcd.drawRect(8, y, TFT_WIDTH - 16, 32, 0xFD20);
            _lcd.setTextColor(0xFD20, 0x2104);
        } else {
            _lcd.fillRect(8, y, TFT_WIDTH - 16, 32, 0x10A2);
            _lcd.drawRect(8, y, TFT_WIDTH - 16, 32, 0x31A6);
            _lcd.setTextColor(0xFFFF, 0x10A2);
        }

        _lcd.setTextSize(1);
        _lcd.drawString(items[i], 20, y + 5);

        // Subtitle / option detail
        if (i == 0) {
            snprintf(subText, sizeof(subText), "[ %s ]", currentPresetName);
            _lcd.setTextColor(isSel ? 0x07FF : 0x7BEF, isSel ? 0x2104 : 0x10A2);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 1) {
            snprintf(subText, sizeof(subText), "[ %d%% ]", (int)((brightnessLevel * 100) / 255));
            _lcd.setTextColor(isSel ? 0x07FF : 0x7BEF, isSel ? 0x2104 : 0x10A2);
            _lcd.drawString(subText, 20, y + 18);
        } else if (i == 2) {
            _lcd.setTextColor(isSel ? 0x07E0 : 0x7BEF, isSel ? 0x2104 : 0x10A2);
            _lcd.drawString("SoftAP Web Download", 20, y + 18);
        }
    }
}

void DisplayUI::drawWifiServerScreen(const char* ssid, const char* ip, uint16_t photoCount) {
    _lcd.fillScreen(0x0000);

    // Wi-Fi Header
    _lcd.fillRect(0, 0, TFT_WIDTH, 34, 0x0010);
    _lcd.drawFastHLine(0, 34, TFT_WIDTH, 0x07E0);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0x07E0, 0x0010);
    _lcd.drawCenterString("WI-FI SHARE", 120, 8);

    _lcd.setTextSize(1);
    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("CONNECT FROM PHONE:", 120, 50);

    // SSID and Password box
    _lcd.fillRect(16, 68, TFT_WIDTH - 32, 54, 0x18E3);
    _lcd.drawRect(16, 68, TFT_WIDTH - 32, 54, 0x07FF);

    _lcd.setTextColor(0xFFFF, 0x18E3);
    _lcd.drawString("SSID: ", 26, 78);
    _lcd.setTextColor(0x07FF, 0x18E3);
    _lcd.drawString(ssid, 68, 78);

    _lcd.setTextColor(0xFFFF, 0x18E3);
    _lcd.drawString("PASS: ", 26, 98);
    _lcd.setTextColor(0x07FF, 0x18E3);
    _lcd.drawString(strlen(WIFI_AP_PASS) > 0 ? WIFI_AP_PASS : "OPEN (NO PASS)", 68, 98);

    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString("OPEN IN BROWSER:", 120, 134);

    // URL / IP box
    _lcd.fillRect(16, 148, TFT_WIDTH - 32, 40, 0x2104);
    _lcd.drawRect(16, 148, TFT_WIDTH - 32, 40, 0x07E0);
    _lcd.setTextSize(1);
    _lcd.setTextColor(0x07E0, 0x2104);
    _lcd.drawCenterString("http://miphoto.local", 120, 155);
    _lcd.setTextColor(0xAD55, 0x2104);
    _lcd.drawCenterString(ip, 120, 172);

    _lcd.setTextSize(1);
    char infoStr[32];
    snprintf(infoStr, sizeof(infoStr), "Photos on SD: %d", photoCount);
    _lcd.setTextColor(0xAD55, 0x0000);
    _lcd.drawCenterString(infoStr, 120, 195);

    _lcd.setTextColor(0xF800, 0x0000);
    _lcd.drawCenterString("[ Press NAV to return ]", 120, 218);
}

void DisplayUI::showMessage(const char* title, const char* subtitle, uint16_t delayMs) {
    _lcd.fillScreen(0x0000);
    _lcd.setTextSize(2);
    _lcd.setTextColor(0xFD20, 0x0000);
    _lcd.drawCenterString(title, 120, 95);
    if (subtitle) {
        _lcd.setTextSize(1);
        _lcd.setTextColor(0xFFFF, 0x0000);
        _lcd.drawCenterString(subtitle, 120, 130);
    }
    if (delayMs > 0) {
        delay(delayMs);
    }
}
