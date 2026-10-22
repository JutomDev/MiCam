#pragma once

#include <Arduino.h>

// ============================================================================
// 1. DVP CAMERA PINOUT (Seeed Studio XIAO ESP32S3 Sense)
// ============================================================================
// Hardware Engineering Note:
// The Sense board camera D5 line on the B2B connector is physically wired to GPIO 12 (Y7).
// GPIO 21 is exclusively dedicated to the MicroSD CS line.
// Using GPIO 21 for camera data would cause a hardware conflict with the SD card SPI bus.
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 10
#define CAM_PIN_SIOD 40  // SDA
#define CAM_PIN_SIOC 39  // SCL

#define CAM_PIN_D7 48  // Y9
#define CAM_PIN_D6 11  // Y8
#define CAM_PIN_D5 12  // Y7 (corrected from 21 to 12)
#define CAM_PIN_D4 14  // Y6
#define CAM_PIN_D3 16  // Y5
#define CAM_PIN_D2 18  // Y4
#define CAM_PIN_D1 17  // Y3
#define CAM_PIN_D0 15  // Y2
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 13

// ============================================================================
// 2. MICROSD CARD PINOUT (Onboard slot on Sense expansion board)
// ============================================================================
// SPI2 Bus (FSPI)
#define SD_CS_PIN 21
#define SD_SCK_PIN 7
#define SD_MISO_PIN 8
#define SD_MOSI_PIN 9
#define SD_SPI_FREQ 25000000  // 25 MHz for stable FAT32 writes

// ============================================================================
// 3. DIGITAL PDM MICROPHONE (Onboard Sense expansion board)
// ============================================================================
// Hardware note: The PDM microphone uses dedicated internal GPIO 41 and 42.
// These pins are NOT exposed on the outer D0-D10 headers, avoiding any conflicts
// with display, button, or peripheral pins.
#define MIC_PDM_DATA_PIN 41
#define MIC_PDM_CLK_PIN 42

// ============================================================================
// 4. ST7789 IPS DISPLAY PINOUT (1.3" 240x240 or 1.69" 240x280)
// ============================================================================
// SPI3 Bus (HSPI) - hardware isolated from SD card SPI2 bus
#define TFT_SCLK_PIN 2  // D1 (GPIO 2)
#define TFT_MOSI_PIN 3  // D2 (GPIO 3)
#define TFT_DC_PIN 4    // D3 (GPIO 4)
#define TFT_RST_PIN -1  // Hardwired to 3.3V / EN
#define TFT_CS_PIN -1   // Hardwired to GND
#define TFT_BLK_PIN -1  // Backlight wired to 3.3V (via 22-47R current limiting resistor)

#define TFT_WIDTH 240
#define TFT_HEIGHT 280  // 1.69" display (240x280); 240x240 frame with bottom 40px HUD
#define PREVIEW_WIDTH 240
#define PREVIEW_HEIGHT 240
#define HUD_BAR_HEIGHT 40      // Dedicated telemetry HUD bar outside active camera frame
#define TFT_SPI_FREQ 40000000  // 40 MHz SPI

// ============================================================================
// 5. 2D ANALOG JOYSTICK, SHUTTER BUTTON, BUZZER AND TALLY LED
// ============================================================================
// Hardware Engineering Note:
// ESP32-S3 pins D6 (GPIO 43) and D7 (GPIO 44) do NOT have hardware ADC capability.
// Therefore, ADC-capable pins (D0, D4) are assigned to joystick X and Y axes,
// while digital pins D6 and D7 are allocated for shutter button and tally LED:
//
// 1. 2D Analog Joystick (PSP-1000 slide joystick / thumb joystick):
#define JOYSTICK_X_PIN 1  // D0 (GPIO 1, ADC1_CH0) - Horizontal axis X
#define JOYSTICK_Y_PIN 5  // D4 (GPIO 5, ADC1_CH4) - Vertical axis Y
#define USE_ANALOG_JOYSTICK 1

// 2. Shutter / OK Button:
#define BTN_SHUTTER_PIN 43  // D6 (GPIO 43, digital INPUT_PULLUP to GND)
#define BTN_NAV_PIN 43      // Shared in FSM or navigated via joystick

// 3. Signal Tally LED (shutter countdown, capture indicator):
// Standard 3mm LED connected through a 1k resistor to GND
#define LED_TALLY_PIN 44  // D7 (GPIO 44, digital OUTPUT)
#define USE_TALLY_LED 1

// 4. Passive Piezo Buzzer (chiptune audio, shutter click, countdown):
#define BUZZER_PIN 6  // D5 (GPIO 6, hardware LEDC PWM Channel 7 to GND)
#define USE_BUZZER 1

#define DEBOUNCE_TIME_MS 30
#define LONG_PRESS_TIME_MS 600

// ============================================================================
// 6. FINITE STATE MACHINE & PRESET ENUMS
// ============================================================================
enum CameraState {
    STATE_LIVE_VIEW,     // Live preview 240x240 RGB565 on ST7789
    STATE_CAPTURE,       // Capture UXGA 1600x1200 JPEG, save to SD, flash animation
    STATE_TIMELAPSE,     // Automated intervalometer (photos every 5s/10s/30s on tripod)
    STATE_BURST_STACK,   // Burst of 4-8 frames with temporal noise averaging
    STATE_RECORD_VIDEO,  // Record MJPEG AVI video to SD card
    STATE_GALLERY,       // Photo gallery viewer for SD card media
    STATE_MENU,          // Settings menu (presets, brightness, Wi-Fi, time-lapse)
    STATE_WIFI_SERVER    // SoftAP access point + HTTP WebServer for photo downloads
};

enum CameraPreset {
    PRESET_AUTO = 0,        // Optimized AWB, AEC2, BPC, WPC, raw gamma and lens correction
    PRESET_BRIGHT_DAY,      // Sunny outdoor, low ISO, enhanced saturation and sharpness
    PRESET_NIGHT,           // Extended exposure, elevated gain, lowered framerate
    PRESET_MACRO_PORTRAIT,  // Softened edge enhancements, higher contrast, warm AWB
    PRESET_RETRO_Y2K,       // 2000s vintage look: grain, nostalgic highlight clipping
    PRESET_LOMO_CROSS,      // Lomography style: cross-processed color profile, strong vignette
    PRESET_COUNT
};

// ============================================================================
// 7. NETWORK CONFIGURATION (SoftAP & WebServer)
// ============================================================================
#define WIFI_AP_SSID "MIphoto-cam"
#define WIFI_AP_PASS ""  // Open Wi-Fi network (no password required)
#define WIFI_AP_CHANNEL 6
#define WIFI_MDNS_HOST "miphoto"  // Access URL: http://miphoto.local

// Boot mode out-of-the-box:
// 1 = automatic SoftAP launch for headless testing without LCD/buttons.
// 0 = standard boot into ST7789 Live View.
#define DEFAULT_BOOT_WIFI_TEST_MODE 1

// ============================================================================
// 8. STORAGE PATHS & FILESYSTEM DEFINITIONS
// ============================================================================
#define PHOTO_DIR "/DCIM"
#define PHOTO_PREFIX "/DCIM/IMG_"
#define PHOTO_EXT ".JPG"
#define VIDEO_PREFIX "/DCIM/VID_"
#define VIDEO_EXT ".AVI"
