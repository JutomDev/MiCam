#pragma once

#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

/**
 * @brief High-performance DVP camera driver for Omnivision OV2640 on ESP32-S3.
 * Manages frame acquisition, PSRAM framebuffers, resolutions, and ISP presets.
 */
class CameraDriver {
public:
    static bool init();
    static bool setModePreview();
    static bool setModeWifiStream();
    static bool setModeCapture();
    static camera_fb_t* getFrame();
    static void returnFrame(camera_fb_t* fb);
    static void applyPreset(CameraPreset preset);
    static const char* getPresetName(CameraPreset preset);
    static CameraPreset getCurrentPreset();
    static void nextPreset();

private:
    static CameraPreset _currentPreset;
    static void flushPipeline(uint8_t count = 2);
};
