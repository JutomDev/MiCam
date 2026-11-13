#include "camera_driver.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
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

    // Keep DMA permanently in 240x240 RGB565 (2 double-buffered framebuffers in PSRAM).
    // Never switch esp_camera to PIXFORMAT_JPEG at runtime, because cam_hal locks jpeg_mode=0
    // at esp_camera_init() time and switching pixel format causes DMA FB-SIZE mismatches and WDT resets.
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_240X240;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return false;
    }

    ESP_LOGI(TAG, "Camera initialized successfully with 240x240 RGB565 buffers.");

    if (!setModePreview()) {
        ESP_LOGE(TAG, "Failed to configure preview mode");
        return false;
    }

    applyPreset(_currentPreset);
    return true;
}

// cppcheck-suppress unusedPrivateFunction
void CameraDriver::flushPipeline(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool CameraDriver::setModePreview() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Direct 240x240 RGB565 uncompressed preview - 1:1 pixel match for ST7789 display
    s->set_pixformat(s, PIXFORMAT_RGB565);
    s->set_framesize(s, FRAMESIZE_240X240);

    // Hardware orientation correction (180 deg flip for housing assembly)
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);

    // Unlock high-speed Live View (~22 FPS instead of 8.3 FPS):
    // ov2640 set_framesize(240X240) defaults CLKRC (Bank 0x01, reg 0x11) to 0x83 (div by 4).
    // Setting CLKRC to 0x81 (PLL 2x ON, divider = 2) doubles the pixel clock safely for 240x240 DMA.
    s->set_reg(s, 0xff, 0xff, 0x01);  // BANK_SENSOR
    s->set_reg(s, 0x11, 0xff, 0x81);  // CLKRC: 2x PLL enabled, clk_div = 1
    s->set_reg(s, 0xff, 0xff, 0x00);  // BANK_DSP

    return true;
}

bool CameraDriver::setModeWifiStream() {
    // Keep DMA in fast 240x240 RGB565 mode and encode JPEG via fmt2jpg() on demand
    return setModePreview();
}

bool CameraDriver::setModeCapture() {
    // Keep DMA in fast 240x240 RGB565 mode to avoid cam_hal FB-SIZE mismatch & WDT hangs
    return setModePreview();
}

camera_fb_t* CameraDriver::getFrame() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb && fb->buf && fb->len >= (size_t)(fb->width * fb->height * 2)) {
        applyPresetEffect(reinterpret_cast<uint16_t*>(fb->buf), (size_t)fb->width * fb->height);
    }
    return fb;
}

void CameraDriver::returnFrame(camera_fb_t* fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

static constexpr size_t STATIC_JPG_BUF_SIZE = 96 * 1024;
static uint8_t* s_staticJpgBuf = nullptr;

struct JpgStreamCtx {
    uint8_t* buf;
    size_t capacity;
    size_t len;
    bool overflow;
};

static size_t jpgOutCallback(void* arg, size_t index, const void* data, size_t len) {
    JpgStreamCtx* ctx = static_cast<JpgStreamCtx*>(arg);
    if (!ctx || !ctx->buf || !data || len == 0) return len;
    if (index + len > ctx->capacity) {
        ctx->overflow = true;
        return 0;
    }
    memcpy(ctx->buf + index, data, len);
    if (index + len > ctx->len) {
        ctx->len = index + len;
    }
    return len;
}

bool CameraDriver::captureJpegStatic(const uint8_t** outJpg, size_t* outLen, uint8_t quality) {
    if (!outJpg || !outLen) return false;
    *outJpg = nullptr;
    *outLen = 0;

    if (!s_staticJpgBuf) {
        s_staticJpgBuf =
            static_cast<uint8_t*>(heap_caps_malloc(STATIC_JPG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_staticJpgBuf) {
            s_staticJpgBuf = static_cast<uint8_t*>(malloc(STATIC_JPG_BUF_SIZE));
        }
        if (!s_staticJpgBuf) {
            ESP_LOGE(TAG, "Failed to allocate static PSRAM JPEG buffer");
            return false;
        }
    }

    camera_fb_t* fb = getFrame();
    if (!fb || !fb->buf || fb->len == 0) {
        if (fb) returnFrame(fb);
        return false;
    }

    JpgStreamCtx ctx = {s_staticJpgBuf, STATIC_JPG_BUF_SIZE, 0, false};
    bool ok = fmt2jpg_cb(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, quality, jpgOutCallback, &ctx);
    returnFrame(fb);

    if (!ok || ctx.overflow || ctx.len == 0) {
        return false;
    }

    *outJpg = s_staticJpgBuf;
    *outLen = ctx.len;
    return true;
}

bool CameraDriver::captureJpeg(uint8_t** outJpg, size_t* outLen, uint8_t quality) {
    if (!outJpg || !outLen) return false;
    *outJpg = nullptr;
    *outLen = 0;

    camera_fb_t* fb = getFrame();
    if (!fb || !fb->buf || fb->len == 0) {
        if (fb) returnFrame(fb);
        return false;
    }

    // Encode RGB565 frame (with active preset styling) into standard JPEG
    bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, quality, outJpg, outLen);
    returnFrame(fb);
    return ok && (*outJpg != nullptr) && (*outLen > 0);
}

void CameraDriver::applyPresetEffect(uint16_t* rgb565_buf, size_t pixelCount) {
    if (!rgb565_buf) return;

    // Fast in-place color grading for stylized presets (big-endian RGB565 from OV2640)
    if (_currentPreset == PRESET_RETRO_Y2K) {
        // Warm golden Y2K tint + crisp contrast
        for (size_t i = 0; i < pixelCount; i++) {
            uint16_t px = rgb565_buf[i];
            uint16_t v = (px >> 8) | (px << 8);
            int r = (v >> 11) & 0x1F;
            int g = (v >> 5) & 0x3F;
            int b = v & 0x1F;
            r = min(31, r + 3);
            g = min(63, g + 2);
            b = max(0, b - 3);
            uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
            rgb565_buf[i] = (out >> 8) | (out << 8);
        }
    } else if (_currentPreset == PRESET_LOMO_CROSS) {
        // High-contrast cross-processed cyan/emerald shadows & punchy reds
        for (size_t i = 0; i < pixelCount; i++) {
            uint16_t px = rgb565_buf[i];
            uint16_t v = (px >> 8) | (px << 8);
            int r = (v >> 11) & 0x1F;
            int g = (v >> 5) & 0x3F;
            int b = v & 0x1F;
            r = (r > 16) ? min(31, r + 4) : max(0, r - 2);
            g = min(63, g + 3);
            b = (b < 16) ? min(31, b + 3) : b;
            uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
            rgb565_buf[i] = (out >> 8) | (out << 8);
        }
    }
}

void CameraDriver::applyPreset(CameraPreset preset) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;

    _currentPreset = preset;

    switch (preset) {
        case PRESET_AUTO:
            s->set_lenc(s, 1);
            s->set_bpc(s, 1);
            s->set_wpc(s, 1);
            s->set_raw_gma(s, 1);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_wb_mode(s, 0);  // Auto WB
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, 0);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);
            s->set_brightness(s, 0);
            s->set_contrast(s, 1);
            s->set_saturation(s, 1);
            s->set_special_effect(s, 0);
            s->set_reg(s, 0xff, 0xff, 0x00);  // BANK_DSP (0x00)
            s->set_reg(s, 0x92, 0xff, 0x01);
            s->set_reg(s, 0x93, 0xff, 0xc2);
            break;

        case PRESET_BRIGHT_DAY:
            s->set_lenc(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 1);  // Sunny
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, -1);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)1);
            s->set_brightness(s, 0);
            s->set_contrast(s, 1);
            s->set_saturation(s, 2);
            s->set_special_effect(s, 0);
            s->set_reg(s, 0xff, 0xff, 0x00);  // BANK_DSP (0x00)
            s->set_reg(s, 0x92, 0xff, 0x01);
            s->set_reg(s, 0x93, 0xff, 0xc6);
            break;

        case PRESET_NIGHT:
            s->set_lenc(s, 1);
            s->set_bpc(s, 1);
            s->set_wpc(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 0);
            s->set_ae_level(s, 2);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)6);
            s->set_brightness(s, 2);
            s->set_contrast(s, 0);
            s->set_saturation(s, 0);
            s->set_special_effect(s, 0);
            s->set_reg(s, 0xff, 0xff, 0x00);  // BANK_DSP (0x00)
            s->set_reg(s, 0x92, 0xff, 0x00);
            break;

        case PRESET_MACRO_PORTRAIT:
            s->set_lenc(s, 1);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)4);
            s->set_brightness(s, 1);
            s->set_contrast(s, 0);
            s->set_saturation(s, 2);
            s->set_special_effect(s, 0);
            s->set_reg(s, 0xff, 0xff, 0x00);  // BANK_DSP (0x00)
            s->set_reg(s, 0x92, 0xff, 0x00);
            break;

        case PRESET_RETRO_Y2K:
            s->set_lenc(s, 0);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 0);
            s->set_ae_level(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);
            s->set_brightness(s, 1);
            s->set_contrast(s, 2);
            s->set_saturation(s, 1);
            s->set_special_effect(s, 0);
            break;

        case PRESET_LOMO_CROSS:
            s->set_lenc(s, 0);
            s->set_raw_gma(s, 1);
            s->set_whitebal(s, 1);
            s->set_wb_mode(s, 3);
            s->set_exposure_ctrl(s, 1);
            s->set_ae_level(s, 0);
            s->set_contrast(s, 2);
            s->set_saturation(s, 2);
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
