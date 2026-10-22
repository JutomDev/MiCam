#include "camera_driver.h"
#include "esp_log.h"

static const char* TAG = "CAM_DRIVER";
CameraPreset CameraDriver::_currentPreset = PRESET_AUTO;

bool CameraDriver::init() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;  // GPIO 12 na Seeed Sense
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;

    // Rigorous DMA Buffer Engineering:
    // Initialize driver with maximum resolution (UXGA 1600x1200) and JPEG in PSRAM,
    // ensuring the DMA controller allocates descriptors and memory pools for the largest frame size.
    // This allows subsequent dynamic mode switching between 240x240 RGB565 preview and UXGA JPEG capture
    // without risking memory reallocation failures or kernel panics.
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return false;
    }

    ESP_LOGI(TAG, "Camera initialized successfully with UXGA PSRAM buffers.");

    // Immediately switch sensor to fast Live View preview mode
    if (!setModePreview()) {
        ESP_LOGE(TAG, "Failed to switch to preview mode");
        return false;
    }

    applyPreset(_currentPreset);
    return true;
}

void CameraDriver::flushPipeline(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

bool CameraDriver::setModePreview() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Direct 240x240 RGB565 uncompressed preview - 1:1 pixel match for ST7789 display
    s->set_pixformat(s, PIXFORMAT_RGB565);
    s->set_framesize(s, FRAMESIZE_240X240);
    flushPipeline(2);
    return true;
}

bool CameraDriver::setModeWifiStream() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Fast JPEG QVGA (320x240) stream for mobile web browser (15-20 FPS)
    s->set_pixformat(s, PIXFORMAT_JPEG);
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 12);
    flushPipeline(2);
    return true;
}

bool CameraDriver::setModeCapture() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Full-resolution UXGA (1600x1200) JPEG capture
    // Quality 8 provides minimal DCT compression and preserves fine optical details
    // while operating well within 8MB Octal PSRAM buffer limits.
    s->set_pixformat(s, PIXFORMAT_JPEG);
    s->set_framesize(s, FRAMESIZE_UXGA);
    s->set_quality(s, 8);
    flushPipeline(2);
    return true;
}

camera_fb_t* CameraDriver::getFrame() {
    return esp_camera_fb_get();
}

void CameraDriver::returnFrame(camera_fb_t* fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

void CameraDriver::applyPreset(CameraPreset preset) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;

    _currentPreset = preset;

    switch (preset) {
        case PRESET_AUTO:
            // Optimized AUTO profile based on empirical sensor analysis:
            // Elevated micro-contrast (+1) mitigates lens flare, +1 saturation balances skin tones
            s->set_lenc(s, 1);      // Lens Shading Correction
            s->set_bpc(s, 1);       // Black Pixel Correction
            s->set_wpc(s, 1);       // White/Hot Pixel Correction
            s->set_raw_gma(s, 1);   // Nonlinear raw gamma curve (shadow retrieval)
            s->set_whitebal(s, 1);  // Automatic White Balance enabled
            s->set_awb_gain(s, 1);
            s->set_wb_mode(s, 0);        // Auto WB
            s->set_exposure_ctrl(s, 1);  // Automatic Exposure Control enabled
            s->set_aec2(s, 1);           // Advanced AEC2 DSP matrix metering
            s->set_ae_level(s, 0);
            s->set_gain_ctrl(s, 1);                   // AGC enabled
            s->set_gainceiling(s, (gainceiling_t)3);  // 8x gain ceiling (shadow detail without severe noise)
            s->set_brightness(s, 0);
            s->set_contrast(s, 1);    // +1 micro-contrast
            s->set_saturation(s, 1);  // +1 natural saturation
            s->set_special_effect(s, 0);
            // Hardware Edge Enhancement on OV2640 DSP (Bank 1, reg 0x92/0x93):
            s->set_reg(s, 0xff, 0xff, 0x01);  // Bank 1
            s->set_reg(s, 0x92, 0xff, 0x01);  // Enable Edge Enhancement
            s->set_reg(s, 0x93, 0xff, 0xc2);  // Moderate detail sharpness
            s->set_reg(s, 0xff, 0xff, 0x00);  // Bank 0
            break;

        case PRESET_BRIGHT_DAY:
            // Bright daylight: highlights preservation against cloud clipping
            s->set_lenc(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 1);  // Sunny (5500K fixed white point)
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, -1);  // -1 protects sky and clouds from 255 clipping
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)0);  // Minimum gain ceiling (2x) for optimal SNR
            s->set_brightness(s, 0);
            s->set_contrast(s, 1);    // Enhanced contrast
            s->set_saturation(s, 2);  // Vibrant foliage and blue sky tones
            s->set_special_effect(s, 0);
            // Hardware edge enhancement for landscape and architecture:
            s->set_reg(s, 0xff, 0xff, 0x01);
            s->set_reg(s, 0x92, 0xff, 0x01);
            s->set_reg(s, 0x93, 0xff, 0xc8);  // Crisp sharpness level
            s->set_reg(s, 0xff, 0xff, 0x00);
            break;

        case PRESET_NIGHT:
            // Low-light / night scenes: maximum exposure and sensitivity
            s->set_lenc(s, 1);
            s->set_bpc(s, 1);
            s->set_wpc(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 0);      // Disabling AEC2 unlocks longest shutter integration
            s->set_ae_level(s, 2);  // Maximum exposure elongation
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)6);  // Maximum analog gain (64x/128x)
            s->set_brightness(s, 2);                  // Lift deep shadows
            s->set_contrast(s, 0);
            s->set_saturation(s, 0);
            s->set_special_effect(s, 0);
            // Disable sharpening in low-light to prevent thermal noise amplification:
            s->set_reg(s, 0xff, 0xff, 0x01);
            s->set_reg(s, 0x92, 0xff, 0x00);
            s->set_reg(s, 0xff, 0xff, 0x00);
            break;

        case PRESET_MACRO_PORTRAIT:
            // Portrait and close-up profile: soft edges, warm skin tones
            s->set_lenc(s, 1);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, 1);  // Slight positive exposure compensation for backlit subjects
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)4);  // 16x gain headroom
            s->set_brightness(s, 1);                  // Gentle lift
            s->set_contrast(s, 1);
            s->set_saturation(s, 2);  // Rich skin tones, eliminating greenish tint
            s->set_special_effect(s, 0);
            // Smooth edge profile (eliminates artificial ringing artifacts around skin):
            s->set_reg(s, 0xff, 0xff, 0x01);
            s->set_reg(s, 0x92, 0xff, 0x00);
            s->set_reg(s, 0xff, 0xff, 0x00);
            break;

        case PRESET_RETRO_Y2K:
            // Vintage early 2000s digicam aesthetic: grain, highlight bloom, punchy contrast
            s->set_lenc(s, 0);  // Authentic corner lens vignetting
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 0);
            s->set_ae_level(s, 1);  // Nostalgic slight highlight clipping
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);  // Early digital sensor noise texture
            s->set_brightness(s, 1);
            s->set_contrast(s, 2);  // Crisp vintage curve
            s->set_saturation(s, 1);
            s->set_special_effect(s, 0);
            break;

        case PRESET_LOMO_CROSS:
            // Lomography cross-process style: rich saturation, vignette, stylized tint
            s->set_lenc(s, 0);  // Heavy natural vignetting
            s->set_raw_gma(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 3);  // Office white balance profile (analog color shift)
            s->set_exposure_ctrl(s, 1);
            s->set_ae_level(s, 0);
            s->set_contrast(s, 2);
            s->set_saturation(s, 2);  // High color saturation
            s->set_special_effect(s, 0);
            break;

        default:
            break;
    }
}

const char* CameraDriver::getPresetName(CameraPreset preset) {
    switch (preset) {
        case PRESET_AUTO:
            return "AUTO ISP";
        case PRESET_BRIGHT_DAY:
            return "BRIGHT DAY";
        case PRESET_NIGHT:
            return "NIGHT";
        case PRESET_MACRO_PORTRAIT:
            return "PORTRAIT";
        case PRESET_RETRO_Y2K:
            return "RETRO Y2K";
        case PRESET_LOMO_CROSS:
            return "LOMOGRAPHY";
        default:
            return "UNKNOWN";
    }
}

CameraPreset CameraDriver::getCurrentPreset() {
    return _currentPreset;
}

void CameraDriver::nextPreset() {
    uint8_t next = ((uint8_t)_currentPreset + 1) % PRESET_COUNT;
    applyPreset((CameraPreset)next);
}
