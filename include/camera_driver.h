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
    static bool captureJpeg(uint8_t** outJpg, size_t* outLen, uint8_t quality = 90);
    static bool captureJpegStatic(const uint8_t** outJpg, size_t* outLen, uint8_t quality = 80);
    static void applyPresetEffect(uint16_t* rgb565_buf, size_t pixelCount);
    static void applyPreset(CameraPreset preset);
    static const char* getPresetName(CameraPreset preset);
    static CameraPreset getCurrentPreset();
    static void nextPreset();

private:
    static CameraPreset _currentPreset;
    // cppcheck-suppress unusedPrivateFunction
    static void flushPipeline(uint8_t count = 2);
};
