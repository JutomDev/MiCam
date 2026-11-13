#include "camera_driver.h"
#include "img_converters.h"
#include <lgfx/utility/lgfx_tjpgd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>

static const char* TAG = "CAM_DRIVER";
CameraPreset CameraDriver::_currentPreset = PRESET_AUTO;

static constexpr size_t STATIC_JPG_BUF_SIZE = 512 * 1024;
static uint8_t* s_staticJpgBuf = nullptr;

enum DriverPipelineMode { DRV_MODE_NONE = 0, DRV_MODE_RGB565_PREVIEW, DRV_MODE_JPEG_HIGHRES };

static DriverPipelineMode s_driverMode = DRV_MODE_NONE;

struct SensorExposureState {
    uint16_t aecLines;
    int gain;
    int yavg;
    int com7;
    int awbR;
    int awbG;
    int awbB;
    bool valid;
    bool awbValid;
};

static void setDummyLines(sensor_t* s, uint16_t lines) {
    if (!s) return;
    // BANK_SENSOR (0x100): ADDVSL (0x2D) and ADDVSH (0x2E) extend vertical blanking for longer exposure
    s->set_reg(s, 0x12D, 0xFF, lines & 0xFF);
    s->set_reg(s, 0x12E, 0xFF, (lines >> 8) & 0xFF);
}

static SensorExposureState saveSensorExposure() {
    SensorExposureState st = {0, 0, 64, 0, 0x52, 0x41, 0x5E, false, false};
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return st;

    st.gain = s->get_reg(s, 0x100, 0xFF);
    int reg04 = s->get_reg(s, 0x104, 0xFF);
    int aec = s->get_reg(s, 0x110, 0xFF);
    int reg45 = s->get_reg(s, 0x145, 0xFF);
    int yavg = s->get_reg(s, 0x12F, 0xFF);
    int com7 = s->get_reg(s, 0x112, 0xFF);

    if (st.gain >= 0 && reg04 >= 0 && aec >= 0 && reg45 >= 0) {
        st.aecLines = static_cast<uint16_t>(((reg45 & 0x3F) << 10) | ((aec & 0xFF) << 2) | (reg04 & 0x03));
        st.yavg = (yavg >= 0) ? yavg : 64;
        st.com7 = (com7 >= 0) ? (com7 & 0x60) : 0x20;
        st.valid = true;
    }

    // Preserve AWB RGB channel multipliers (BANK_DSP 0x0CC..0x0CE) converged in Live View across esp_camera_init()
    int regC7 = s->get_reg(s, 0x0C7, 0x40);
    int awbR = s->get_reg(s, 0x0CC, 0xFF);
    int awbG = s->get_reg(s, 0x0CD, 0xFF);
    int awbB = s->get_reg(s, 0x0CE, 0xFF);
    if (regC7 == 0 && awbR > 0 && awbG > 0 && awbB > 0) {
        st.awbR = awbR;
        st.awbG = awbG;
        st.awbB = awbB;
        st.awbValid = true;
    }
    return st;
}

static void restoreSensorExposure(const SensorExposureState& st, int targetCom7 = -1) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;

    const CameraPreset active = CameraDriver::getCurrentPreset();
    const bool usesAutoWb = (active == PRESET_AUTO || active == PRESET_NIGHT || active == PRESET_MACRO_PORTRAIT);
    if (st.awbValid && usesAutoWb) {
        s->set_reg(s, 0x0CC, 0xFF, st.awbR);
        s->set_reg(s, 0x0CD, 0xFF, st.awbG);
        s->set_reg(s, 0x0CE, 0xFF, st.awbB);
    }

    if (!st.valid) return;

    int dstCom7 = (targetCom7 >= 0) ? (targetCom7 & 0x60) : (s->get_reg(s, 0x112, 0x60));
    uint32_t scaled = st.aecLines;

    // Scale AEC integration line count across CIF (296 lines) <-> SVGA (600 lines) <-> UXGA (1200 lines)
    if (st.com7 == 0x20 && dstCom7 == 0x00) {
        scaled *= 4U;
    } else if (st.com7 == 0x20 && dstCom7 == 0x40) {
        scaled *= 2U;
    } else if (st.com7 == 0x40 && dstCom7 == 0x00) {
        scaled *= 2U;
    } else if (st.com7 == 0x00 && dstCom7 == 0x20) {
        scaled = (scaled + 2U) / 4U;
    } else if (st.com7 == 0x40 && dstCom7 == 0x20) {
        scaled = (scaled + 1U) / 2U;
    } else if (st.com7 == 0x00 && dstCom7 == 0x40) {
        scaled = (scaled + 1U) / 2U;
    }

    uint16_t maxLines = (dstCom7 == 0x00) ? 1200 : ((dstCom7 == 0x40) ? 600 : 296);
    if (scaled < 8U) scaled = 8U;
    if (scaled > maxLines) scaled = maxLines;

    s->set_reg(s, 0x100, 0xFF, st.gain);
    s->set_reg(s, 0x104, 0x03, scaled & 0x03);
    s->set_reg(s, 0x110, 0xFF, (scaled >> 2) & 0xFF);
    s->set_reg(s, 0x145, 0x3F, (scaled >> 10) & 0x3F);
}

static bool initHardwarePipeline(DriverPipelineMode targetMode) {
    if (s_driverMode == targetMode) {
        return true;
    }

    SensorExposureState exp = (s_driverMode != DRV_MODE_NONE)
                                  ? saveSensorExposure()
                                  : SensorExposureState{0, 0, 64, 0x20, 0x52, 0x41, 0x5E, false, false};

    if (s_driverMode != DRV_MODE_NONE) {
        esp_camera_deinit();
        s_driverMode = DRV_MODE_NONE;
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    camera_config_t config;
    memset(&config, 0, sizeof(config));
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
    config.xclk_freq_hz = (targetMode == DRV_MODE_RGB565_PREVIEW) ? 16000000 : 20000000;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    if (targetMode == DRV_MODE_RGB565_PREVIEW) {
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_240X240;
        config.jpeg_quality = 10;
    } else {
        config.pixel_format = PIXFORMAT_JPEG;
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 6;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init(mode=%d) failed: 0x%x", (int)targetMode, err);
        s_driverMode = DRV_MODE_NONE;
        return false;
    }

    s_driverMode = targetMode;

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, CAM_VFLIP);
        s->set_hmirror(s, CAM_HMIRROR);
        // Enable 50Hz European mains anti-banding filter (COM3 bit 2)
        s->set_reg(s, 0x10C, 0x06, 0x04);
    }
    CameraDriver::applyPreset(CameraDriver::getCurrentPreset());
    restoreSensorExposure(exp, (targetMode == DRV_MODE_RGB565_PREVIEW) ? 0x20 : 0x00);

    ESP_LOGI(TAG, "Pipeline switched to mode %d | PSRAM free: %u", (int)targetMode, ESP.getFreePsram());
    return true;
}

uint8_t* CameraDriver::getStaticBuffer(size_t* outCapacity) {
    if (!s_staticJpgBuf) {
        s_staticJpgBuf =
            static_cast<uint8_t*>(heap_caps_malloc(STATIC_JPG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_staticJpgBuf) {
            ESP_LOGE(TAG, "Failed to allocate 512KB static PSRAM buffer!");
            if (outCapacity) *outCapacity = 0;
            return nullptr;
        }
        ESP_LOGI(TAG, "Allocated 512 KB static PSRAM buffer");
    }
    if (outCapacity) {
        *outCapacity = STATIC_JPG_BUF_SIZE;
    }
    return s_staticJpgBuf;
}

bool CameraDriver::init() {
    size_t cap = 0;
    getStaticBuffer(&cap);

    if (!initHardwarePipeline(DRV_MODE_RGB565_PREVIEW)) {
        return false;
    }

    if (!setModePreview()) {
        ESP_LOGE(TAG, "Failed to configure preview mode");
        return false;
    }

    applyPreset(_currentPreset);
    ESP_LOGI(TAG, "Camera initialized (RGB565 preview + UXGA/SVGA-Night JPEG pipeline ready).");
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
    if (!initHardwarePipeline(DRV_MODE_RGB565_PREVIEW)) {
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Proven CIF-derived 240x240 RGB565 framesize so OV2640 line FIFO matches CLKRC=0x81
    s->set_pixformat(s, PIXFORMAT_RGB565);
    s->set_framesize(s, FRAMESIZE_240X240);

    // Hardware orientation correction & clear any night dummy lines before fast preview
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
    setDummyLines(s, 0);

    // BANK_SENSOR (0x100) + CLKRC (0x11): 2x PLL ON, clk_div = 1 -> ~22 FPS Live View
    s->set_reg(s, 0x111, 0xFF, 0x81);

    applyPreset(_currentPreset);
    return true;
}

bool CameraDriver::setModeWifiStream() {
    if (!initHardwarePipeline(DRV_MODE_JPEG_HIGHRES)) {
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    // Hardware JPEG VGA (640x480) stream for crisp, zero-CPU Web viewfinder
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_quality(s, 10);
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
    setDummyLines(s, 0);

    applyPreset(_currentPreset);

    // Calm pixel clock divider during Wi-Fi operations so Core 0 DMA ISR rate stays gentle
    s->set_reg(s, 0x111, 0xFF, 0x01);

    flushPipeline(2);
    return true;
}

bool CameraDriver::setModeCapture() {
    if (!initHardwarePipeline(DRV_MODE_JPEG_HIGHRES)) {
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) return false;

    s->set_framesize(s, FRAMESIZE_UXGA);
    s->set_quality(s, 6);
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
    applyPreset(_currentPreset);
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

bool CameraDriver::captureStreamJpegStatic(const uint8_t** outJpg, size_t* outLen) {
    if (!outJpg || !outLen) return false;
    *outJpg = nullptr;
    *outLen = 0;

    size_t cap = 0;
    uint8_t* buf = getStaticBuffer(&cap);
    if (!buf || cap == 0) return false;

    if (s_driverMode != DRV_MODE_JPEG_HIGHRES) {
        if (!setModeWifiStream()) return false;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb || !fb->buf || fb->len == 0 || fb->len > cap) {
        if (fb) esp_camera_fb_return(fb);
        return false;
    }

    memcpy(buf, fb->buf, fb->len);
    *outJpg = buf;
    *outLen = fb->len;
    esp_camera_fb_return(fb);
    return true;
}

struct JpgStaticSink {
    uint8_t* dst;
    size_t cap;
    size_t len;
    bool overflow;
};

static size_t jpgStaticCallback(void* arg, size_t index, const void* data, size_t len) {
    JpgStaticSink* sink = static_cast<JpgStaticSink*>(arg);
    if (!sink || !sink->dst || sink->overflow) return 0;
    // jpge::process_end_of_image() calls put_buf(NULL, 0) as an EOF flush; callback_stream forwards NULL here.
    if (!data || len == 0) return 0;
    if (index + len > sink->cap) {
        sink->overflow = true;
        return 0;
    }
    memcpy(sink->dst + index, data, len);
    if (index + len > sink->len) {
        sink->len = index + len;
    }
    if ((index & 0x3FFFU) == 0U) {
        vTaskDelay(1);
    }
    return len;
}

struct NightBurstContext {
    const uint8_t* srcData;
    size_t srcLen;
    size_t srcPos;
    uint8_t* refY;
    uint16_t* accumRgb;
    uint16_t width;
    uint16_t height;
    uint8_t frameIdx;
    bool valid;
};

// Allocated in PSRAM instead of loopTask's 8 KB stack so LovyanGFX TJpgD & jpge never overflow stack canary.
struct NightScratch {
    alignas(4) uint8_t jdPool[4096];
    uint32_t hist[256];
    uint8_t toneLut[1024];
};

static uint32_t lgfxJpgReaderCb(void* dev, uint8_t* buf, uint32_t len) {
    NightBurstContext* ctx = static_cast<NightBurstContext*>(dev);
    if (!ctx || !ctx->srcData || ctx->srcPos >= ctx->srcLen) return 0;
    size_t avail = ctx->srcLen - ctx->srcPos;
    uint32_t step = (len > avail) ? static_cast<uint32_t>(avail) : len;
    if (buf && step > 0) {
        memcpy(buf, ctx->srcData + ctx->srcPos, step);
    }
    ctx->srcPos += step;
    return step;
}

// Signature mandated by lgfx_jd_decomp callback ABI (non-const bitmap/rect)
// cppcheck-suppress constParameterCallback
static uint32_t lgfxNightDecodeWriter(void* dev, void* bitmap, JRECT* rect) {
    NightBurstContext* ctx = static_cast<NightBurstContext*>(dev);
    if (!ctx || !ctx->valid || !ctx->refY || !ctx->accumRgb || !bitmap || !rect) return 0;

    const uint8_t* data = static_cast<const uint8_t*>(bitmap);
    const uint16_t x = static_cast<uint16_t>(rect->left);
    const uint16_t y = static_cast<uint16_t>(rect->top);
    const uint16_t w = static_cast<uint16_t>(rect->right - rect->left + 1U);
    const uint16_t h = static_cast<uint16_t>(rect->bottom - rect->top + 1U);

    const uint16_t maxW = ctx->width;
    const uint16_t maxH = ctx->height;
    const uint8_t fIdx = ctx->frameIdx;

    for (uint16_t row = 0; row < h; ++row) {
        uint16_t py = y + row;
        if (py >= maxH) break;
        const uint8_t* srcRow = data + static_cast<size_t>(row) * w * 3;
        uint32_t rowBasePx = static_cast<uint32_t>(py) * maxW + x;

        for (uint16_t col = 0; col < w; ++col) {
            if ((x + col) >= maxW) break;
            uint32_t pxIdx = rowBasePx + col;
            uint32_t idx3 = pxIdx * 3U;
            uint8_t r = srcRow[col * 3U];
            uint8_t g = srcRow[col * 3U + 1U];
            uint8_t b = srcRow[col * 3U + 2U];
            uint8_t y8 = static_cast<uint8_t>(
                (static_cast<uint32_t>(r) * 77U + static_cast<uint32_t>(g) * 150U + static_cast<uint32_t>(b) * 29U) >>
                8);

            if (fIdx == 0) {
                ctx->refY[pxIdx] = y8;
                // Weight 2 stored in top 4 bits of R accumulator for slight sharp-anchor bias
                ctx->accumRgb[idx3] = static_cast<uint16_t>((2U << 12) | (static_cast<uint16_t>(r) * 2U));
                ctx->accumRgb[idx3 + 1U] = static_cast<uint16_t>(g) * 2U;
                ctx->accumRgb[idx3 + 2U] = static_cast<uint16_t>(b) * 2U;
            } else {
                int dy = abs(static_cast<int>(y8) - static_cast<int>(ctx->refY[pxIdx]));
                // Motion-robust ghosting guard: blend noise fluctuations while keeping moving foreground sharp
                if (dy <= 28) {
                    uint16_t packedR = ctx->accumRgb[idx3];
                    uint16_t cnt = (packedR >> 12) & 0x0FU;
                    if (cnt < 8U) {
                        ctx->accumRgb[idx3] = static_cast<uint16_t>(((cnt + 1U) << 12) | ((packedR & 0x0FFFU) + r));
                        ctx->accumRgb[idx3 + 1U] += g;
                        ctx->accumRgb[idx3 + 2U] += b;
                    }
                }
            }
        }
    }
    if (x == 0 && (y & 0x3FU) == 0U) {
        vTaskDelay(1);
    }
    return 1;
}

__attribute__((noinline)) static bool decodeNightFrameNoInline(NightBurstContext* ctx, uint8_t* jdPool,
                                                               size_t poolSize) {
    if (!ctx || !jdPool || poolSize == 0) return false;
    lgfxJdec jpegdec;
    ctx->srcPos = 0;
    ctx->valid = false;
    JRESULT res = lgfx_jd_prepare(&jpegdec, lgfxJpgReaderCb, jdPool, static_cast<uint_fast16_t>(poolSize), ctx);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "Night burst jd_prepare failed: %d", static_cast<int>(res));
        return false;
    }
    if (jpegdec.width != ctx->width || jpegdec.height != ctx->height) {
        ESP_LOGW(TAG, "Night burst dimension mismatch: got %ux%u, expected %ux%u", static_cast<unsigned>(jpegdec.width),
                 static_cast<unsigned>(jpegdec.height), ctx->width, ctx->height);
        return false;
    }
    ctx->valid = true;
    res = lgfx_jd_decomp(&jpegdec, lgfxNightDecodeWriter, 0);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "Night burst jd_decomp failed: %d", static_cast<int>(res));
        ctx->valid = false;
        return false;
    }
    return true;
}

__attribute__((noinline)) static bool encodeNightJpegNoInline(uint8_t* rgbBuf, uint16_t width, uint16_t height,
                                                              uint8_t quality, JpgStaticSink* sink) {
    const size_t pxBytes = static_cast<size_t>(width) * height * 3U;
    return fmt2jpg_cb(rgbBuf, pxBytes, width, height, PIXFORMAT_RGB888, quality, jpgStaticCallback, sink) &&
           !sink->overflow && sink->len > 0;
}

__attribute__((noinline)) static void processNightStackInPlace(uint8_t* outBgr, const uint16_t* accumRgb,
                                                               uint32_t pixelCount, NightScratch* scratch) {
    if (!outBgr || !accumRgb || !scratch) return;

    // Pass 1: Subsampled 10-bit statistics (black floor, anti-green WB, scene mean luminance)
    memset(scratch->hist, 0, sizeof(scratch->hist));
    uint64_t sumR = 0, sumG = 0, sumB = 0, sumY = 0;
    uint32_t sampleCount = 0;

    for (uint32_t p = 0; p < pixelCount; p += 4U) {
        uint32_t idx3 = p * 3U;
        uint16_t packedR = accumRgb[idx3];
        uint32_t cnt = (packedR >> 12) & 0x0FU;
        if (cnt == 0U) cnt = 1U;

        uint32_t r10 = ((packedR & 0x0FFFU) * 4U) / cnt;
        uint32_t g10 = (static_cast<uint32_t>(accumRgb[idx3 + 1U]) * 4U) / cnt;
        uint32_t b10 = (static_cast<uint32_t>(accumRgb[idx3 + 2U]) * 4U) / cnt;
        if (r10 > 1020U) r10 = 1020U;
        if (g10 > 1020U) g10 = 1020U;
        if (b10 > 1020U) b10 = 1020U;

        uint32_t y10 = (r10 * 77U + g10 * 150U + b10 * 29U) >> 8;
        scratch->hist[(y10 >> 2) & 0xFFU]++;
        sumR += r10;
        sumG += g10;
        sumB += b10;
        sumY += (y10 >> 2);
        sampleCount++;
    }

    // Yield briefly so Idle/TWDT and Wi-Fi tasks remain happy between bursts and tone mapping
    vTaskDelay(1);

    uint32_t p02Target = sampleCount / 50U;
    uint32_t cum = 0;
    uint32_t p02Bin = 0;
    for (uint32_t b = 0; b < 256U; ++b) {
        cum += scratch->hist[b];
        if (cum >= p02Target) {
            p02Bin = b;
            break;
        }
    }

    // Clamp black-point subtraction safely so dim detail isn't crushed while dark-current haze disappears
    uint32_t floor10 = p02Bin * 3U;
    if (floor10 > 36U) floor10 = 36U;

    float meanY = (sampleCount > 0U) ? (static_cast<float>(sumY) / sampleCount) : 40.0f;
    float avgR = (sampleCount > 0U) ? (static_cast<float>(sumR) / sampleCount) : 200.0f;
    float avgG = (sampleCount > 0U) ? (static_cast<float>(sumG) / sampleCount) : 200.0f;
    float avgB = (sampleCount > 0U) ? (static_cast<float>(sumB) / sampleCount) : 200.0f;
    float grayMean = (avgR + avgG + avgB) / 3.0f;

    // Gentle Grey-World blend (35%) + subtle indoor CMOS green-floor cleanup
    auto clampGain = [](float g) -> uint16_t {
        if (g < 0.88f) g = 0.88f;
        if (g > 1.24f) g = 1.24f;
        return static_cast<uint16_t>(lroundf(g * 256.0f));
    };
    float rawR = (avgR > 16.0f) ? (grayMean / avgR) : 1.0f;
    float rawG = (avgG > 16.0f) ? (grayMean / avgG) : 1.0f;
    float rawB = (avgB > 16.0f) ? (grayMean / avgB) : 1.0f;
    float gBias = (avgG > avgR * 0.96f && avgG > avgB) ? 0.95f : 1.0f;

    const uint16_t gainR_q8 = clampGain(1.0f + 0.35f * (rawR - 1.0f));
    const uint16_t gainG_q8 = clampGain((1.0f + 0.35f * (rawG - 1.0f)) * gBias);
    const uint16_t gainB_q8 = clampGain(1.0f + 0.35f * (rawB - 1.0f));

    // Adaptive Reinhard shadow-lift curve: lifts dark faces cleanly without clipping highlights
    float kLift = 0.7f;
    if (meanY < 92.0f) {
        kLift = 0.7f + (92.0f - meanY) * 0.032f;  // Up to ~3.4 for very dark rooms (meanY ~ 8)
    }
    if (kLift > 3.4f) kLift = 3.4f;

    const float denom = 1023.0f - static_cast<float>(floor10);
    for (uint32_t i = 0; i < 1024U; ++i) {
        if (i <= floor10) {
            scratch->toneLut[i] = 0;
        } else {
            float x = static_cast<float>(i - floor10) / denom;
            float y = ((1.0f + kLift) * x) / (1.0f + kLift * x);
            int val = static_cast<int>(lroundf(y * 255.0f));
            scratch->toneLut[i] = static_cast<uint8_t>((val < 0) ? 0 : ((val > 255) ? 255 : val));
        }
    }

    // Pass 2: Apply 10-bit black-floor, WB gains, and LUT into BGR888 layout in-place (3*p+2 < 6*(p+1))
    for (uint32_t p = 0; p < pixelCount; ++p) {
        if ((p & 0x1FFFFU) == 0x10000U) {
            vTaskDelay(1);
        }
        uint32_t idx3 = p * 3U;
        uint16_t packedR = accumRgb[idx3];
        uint16_t accG = accumRgb[idx3 + 1U];
        uint16_t accB = accumRgb[idx3 + 2U];
        uint32_t cnt = (packedR >> 12) & 0x0FU;
        if (cnt == 0U) cnt = 1U;

        uint32_t r10 = (((packedR & 0x0FFFU) * 4U) / cnt) * gainR_q8 >> 8;
        uint32_t g10 = ((static_cast<uint32_t>(accG) * 4U) / cnt) * gainG_q8 >> 8;
        uint32_t b10 = ((static_cast<uint32_t>(accB) * 4U) / cnt) * gainB_q8 >> 8;

        if (r10 > 1023U) r10 = 1023U;
        if (g10 > 1023U) g10 = 1023U;
        if (b10 > 1023U) b10 = 1023U;

        outBgr[idx3] = scratch->toneLut[b10];
        outBgr[idx3 + 1U] = scratch->toneLut[g10];
        outBgr[idx3 + 2U] = scratch->toneLut[r10];
    }

    vTaskDelay(1);
    ESP_LOGI(TAG, "Night stack complete | meanY=%.1f floor10=%u kLift=%.2f", meanY, (unsigned)floor10, kLift);
}

bool CameraDriver::captureNightBurstStatic(const uint8_t** outJpg, size_t* outLen, uint8_t quality) {
    (void)quality;
    if (!outJpg || !outLen) return false;
    *outJpg = nullptr;
    *outLen = 0;

    size_t cap = 0;
    uint8_t* jpgOutBuf = getStaticBuffer(&cap);
    if (!jpgOutBuf || cap == 0) return false;

    // SVGA (800x600) uses native OV2640 2x2 VarioPixel binning (1600/2 x 1200/2) + 1B/px refY (3.36 MB PSRAM total)
    static constexpr uint16_t NIGHT_W = 800;
    static constexpr uint16_t NIGHT_H = 600;
    static constexpr uint32_t NIGHT_PX = static_cast<uint32_t>(NIGHT_W) * NIGHT_H;
    static constexpr uint8_t BURST_FRAMES = 5;

    bool wasPreview = (s_driverMode == DRV_MODE_RGB565_PREVIEW);
    SensorExposureState exp = saveSensorExposure();

    bool captured = false;
    uint8_t* refY = nullptr;
    uint16_t* accumRgb = nullptr;
    NightScratch* scratch = nullptr;

    if (s_driverMode == DRV_MODE_JPEG_HIGHRES || initHardwarePipeline(DRV_MODE_JPEG_HIGHRES)) {
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            // Switch to native SVGA (2x2 pixel binning) + +40% integration lines before allocating burst buffers
            s->set_framesize(s, FRAMESIZE_SVGA);
            s->set_quality(s, 6);
            s->set_vflip(s, CAM_VFLIP);
            s->set_hmirror(s, CAM_HMIRROR);
            applyPreset(PRESET_NIGHT);
            setDummyLines(s, 240);
            restoreSensorExposure(exp, 0x40);

            flushPipeline(3);

            refY = static_cast<uint8_t*>(heap_caps_malloc(NIGHT_PX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            accumRgb = static_cast<uint16_t*>(
                heap_caps_malloc(NIGHT_PX * 3U * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            scratch =
                static_cast<NightScratch*>(heap_caps_malloc(sizeof(NightScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (refY && accumRgb && scratch) {
                uint8_t decodedOk = 0;

                for (uint8_t i = 0; i < BURST_FRAMES; ++i) {
                    camera_fb_t* fb = esp_camera_fb_get();
                    if (!fb) {
                        vTaskDelay(pdMS_TO_TICKS(30));
                        fb = esp_camera_fb_get();
                    }
                    if (!fb || !fb->buf || fb->len == 0) {
                        if (fb) esp_camera_fb_return(fb);
                        continue;
                    }

                    NightBurstContext ctx = {fb->buf, fb->len, 0, refY, accumRgb, NIGHT_W, NIGHT_H, decodedOk, false};
                    bool ok = decodeNightFrameNoInline(&ctx, scratch->jdPool, sizeof(scratch->jdPool));
                    esp_camera_fb_return(fb);

                    if (ok) {
                        decodedOk++;
                    }
                    vTaskDelay(1);
                }

                free(refY);
                refY = nullptr;

                if (decodedOk > 0) {
                    uint8_t* outBgr = reinterpret_cast<uint8_t*>(accumRgb);
                    processNightStackInPlace(outBgr, accumRgb, NIGHT_PX, scratch);
                    free(scratch);
                    scratch = nullptr;

                    JpgStaticSink sink = {jpgOutBuf, cap, 0, false};
                    uint8_t jpgQ = 94;
                    if (encodeNightJpegNoInline(outBgr, NIGHT_W, NIGHT_H, jpgQ, &sink)) {
                        *outJpg = jpgOutBuf;
                        *outLen = sink.len;
                        captured = true;
                        ESP_LOGI(TAG, "Captured Night Burst (%u frames SVGA-bin, refY): %u bytes", (unsigned)decodedOk,
                                 (unsigned)sink.len);
                    }
                    free(accumRgb);
                    accumRgb = nullptr;
                }
            }

            // Fallback to single long-exposure SVGA JPEG if PSRAM burst buffers weren't available
            if (!captured) {
                camera_fb_t* fb = esp_camera_fb_get();
                if (fb && fb->buf && fb->len > 0 && fb->len <= cap) {
                    memcpy(jpgOutBuf, fb->buf, fb->len);
                    *outJpg = jpgOutBuf;
                    *outLen = fb->len;
                    captured = true;
                    ESP_LOGW(TAG, "Single-frame SVGA night fallback: %u bytes", (unsigned)fb->len);
                }
                if (fb) esp_camera_fb_return(fb);
            }
        }
    }

    if (scratch) free(scratch);
    if (accumRgb) free(accumRgb);
    if (refY) free(refY);

    if (wasPreview) {
        setModePreview();
        restoreSensorExposure(exp, 0x20);
    } else {
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            setDummyLines(s, 0);
            s->set_framesize(s, FRAMESIZE_VGA);
            s->set_quality(s, 10);
            s->set_vflip(s, CAM_VFLIP);
            s->set_hmirror(s, CAM_HMIRROR);
            applyPreset(_currentPreset);
            restoreSensorExposure(exp, 0x40);
            s->set_reg(s, 0x111, 0xFF, 0x01);
            flushPipeline(2);
        }
    }

    return captured;
}

bool CameraDriver::captureJpegStatic(const uint8_t** outJpg, size_t* outLen, uint8_t quality) {
    if (!outJpg || !outLen) return false;
    if (_currentPreset == PRESET_NIGHT) {
        return captureNightBurstStatic(outJpg, outLen, quality);
    }

    *outJpg = nullptr;
    *outLen = 0;

    size_t cap = 0;
    uint8_t* buf = getStaticBuffer(&cap);
    if (!buf || cap == 0) return false;

    bool wasPreview = (s_driverMode == DRV_MODE_RGB565_PREVIEW);
    SensorExposureState exp = saveSensorExposure();

    bool captured = false;
    if (s_driverMode == DRV_MODE_JPEG_HIGHRES || initHardwarePipeline(DRV_MODE_JPEG_HIGHRES)) {
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            // Full 2.0 MP UXGA (1600x1200) Hardware JPEG capture
            s->set_framesize(s, FRAMESIZE_UXGA);
            s->set_quality(s, 6);
            s->set_vflip(s, CAM_VFLIP);
            s->set_hmirror(s, CAM_HMIRROR);
            applyPreset(_currentPreset);
            // In dim indoor rooms (low YAVG), add gentle extra integration lines without gray fog
            setDummyLines(s, (exp.valid && exp.yavg < 45) ? 260 : 0);
            restoreSensorExposure(exp, 0x00);

            // Discard 4 frames so OV2640 AWB & AEC settle completely after resolution/mode change (prevents green cast)
            flushPipeline(4);

            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(30));
                fb = esp_camera_fb_get();
            }

            if (fb && fb->buf && fb->len > 0 && fb->len <= cap) {
                memcpy(buf, fb->buf, fb->len);
                *outJpg = buf;
                *outLen = fb->len;
                captured = true;
                ESP_LOGI(TAG, "Captured 2MP UXGA JPEG: %u bytes (%ux%u)", (unsigned)fb->len, fb->width, fb->height);
            } else {
                ESP_LOGE(TAG, "UXGA capture failed or exceeded static PSRAM buffer (len=%u)",
                         fb ? (unsigned)fb->len : 0);
            }

            if (fb) {
                esp_camera_fb_return(fb);
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to switch to DRV_MODE_JPEG_HIGHRES, falling back to RGB565->JPEG");
    }

    // Restore previous active mode
    if (wasPreview) {
        setModePreview();
        restoreSensorExposure(exp, 0x20);

        if (!captured) {
            camera_fb_t* previewFb = esp_camera_fb_get();
            if (previewFb) {
                JpgStaticSink sink = {buf, cap, 0, false};
                uint8_t fbQuality = (quality >= 10 && quality <= 95) ? quality : 85;
                if (fmt2jpg_cb(previewFb->buf, previewFb->len, previewFb->width, previewFb->height, previewFb->format,
                               fbQuality, jpgStaticCallback, &sink) &&
                    !sink.overflow && sink.len > 0) {
                    *outJpg = buf;
                    *outLen = sink.len;
                    captured = true;
                    ESP_LOGW(TAG, "Fallback RGB565->JPEG capture succeeded: %u bytes", (unsigned)sink.len);
                }
                esp_camera_fb_return(previewFb);
            }
        }
    } else {
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            setDummyLines(s, 0);
            s->set_framesize(s, FRAMESIZE_VGA);
            s->set_quality(s, 10);
            s->set_vflip(s, CAM_VFLIP);
            s->set_hmirror(s, CAM_HMIRROR);
            applyPreset(_currentPreset);
            restoreSensorExposure(exp, 0x40);
            s->set_reg(s, 0x111, 0xFF, 0x01);
            flushPipeline(2);
        }
    }

    return captured;
}

bool CameraDriver::captureJpeg(uint8_t** outJpg, size_t* outLen, uint8_t quality) {
    if (!outJpg || !outLen) return false;
    *outJpg = nullptr;
    *outLen = 0;

    const uint8_t* staticPtr = nullptr;
    size_t len = 0;
    if (!captureJpegStatic(&staticPtr, &len, quality)) {
        return false;
    }

    uint8_t* copyBuf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copyBuf) {
        copyBuf = static_cast<uint8_t*>(malloc(len));
    }
    if (!copyBuf) return false;

    memcpy(copyBuf, staticPtr, len);
    *outJpg = copyBuf;
    *outLen = len;
    return true;
}

void CameraDriver::applyPresetEffect(uint16_t* rgb565_buf, size_t pixelCount) {
    // Hardware DSP registers handle color grading consistently across RGB565 and JPEG.
    (void)rgb565_buf;
    (void)pixelCount;
}

void CameraDriver::applyPreset(CameraPreset preset) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;

    const CameraPreset prevPreset = _currentPreset;
    _currentPreset = preset;

    // Reset manual AWB RGB lock (0x0C7 bit 6) and seed neutral gains only when exiting manual-WB presets.
    // Preserving 0x0CC..0x0CE during same-preset/auto-WB transitions avoids flashing yellow Sunny (0x5E/0x41/0x54).
    const bool prevWasManualWb =
        (prevPreset == PRESET_BRIGHT_DAY || prevPreset == PRESET_RETRO_Y2K || prevPreset == PRESET_LOMO_CROSS);
    const bool nextIsAutoWb = (preset == PRESET_AUTO || preset == PRESET_NIGHT || preset == PRESET_MACRO_PORTRAIT);
    if (prevWasManualWb && nextIsAutoWb) {
        s->set_reg(s, 0x0C7, 0x40, 0x00);
        s->set_reg(s, 0x0CC, 0xFF, 0x52);
        s->set_reg(s, 0x0CD, 0xFF, 0x41);
        s->set_reg(s, 0x0CE, 0xFF, 0x5E);
    }

    // Keep brightness at 0 across presets so DSP doesn't inject flat gray DC fog into shadows
    s->set_lenc(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_special_effect(s, 0);
    s->set_brightness(s, 0);

    // Tunable ranges per OV2640 driver:
    //   wb_mode:     0=Auto, 1=Sunny, 2=Cloudy, 3=Office, 4=Home
    //   ae_level:    -2 .. +2 (target exposure bias)
    //   gainceiling: 0=2x, 1=4x, 2=8x, 3=16x, 4=32x, 5=64x, 6=128x (max ISO)
    //   contrast:    -2 .. +2
    //   saturation:  -2 .. +2
    switch (preset) {
        case PRESET_AUTO:
            s->set_wb_mode(s, 0);
            s->set_ae_level(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);  // 16x max AGC for low noise
            s->set_contrast(s, 1);
            s->set_saturation(s, 1);
            break;

        case PRESET_BRIGHT_DAY:
            s->set_wb_mode(s, 1);  // Sunny outdoor balance
            s->set_ae_level(s, -1);
            s->set_gainceiling(s, (gainceiling_t)1);  // 4x crisp daylight
            s->set_contrast(s, 1);
            s->set_saturation(s, 1);
            break;

        case PRESET_NIGHT:
            s->set_wb_mode(s, 0);  // Auto WB (manual anti-green + shadow lift handled cleanly by Night Burst)
            s->set_ae_level(s, 2);
            s->set_gainceiling(s, (gainceiling_t)4);  // 32x limit prevents 128x sensor speckle haze
            s->set_contrast(s, 0);
            s->set_saturation(s, 1);
            break;

        case PRESET_MACRO_PORTRAIT:
            s->set_wb_mode(s, 0);
            s->set_ae_level(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);  // 16x
            s->set_contrast(s, 0);
            s->set_saturation(s, 1);
            break;

        case PRESET_RETRO_Y2K:
            s->set_lenc(s, 0);  // Subtle natural lens vignette
            s->set_wb_mode(s, 1);
            s->set_ae_level(s, 1);
            s->set_gainceiling(s, (gainceiling_t)3);
            s->set_contrast(s, 1);
            s->set_saturation(s, 1);
            // Warm golden Y2K Kodak tint (manual RGB gains in BANK_DSP 0x0C7..0x0CE)
            s->set_reg(s, 0x0C7, 0x40, 0x40);  // Enable manual AWB gain lock
            s->set_reg(s, 0x0CC, 0xFF, 0x66);  // Red gain   (neutral 0x5E)
            s->set_reg(s, 0x0CD, 0xFF, 0x42);  // Green gain (neutral 0x41)
            s->set_reg(s, 0x0CE, 0xFF, 0x3E);  // Blue gain  (neutral 0x54)
            break;

        case PRESET_LOMO_CROSS:
            s->set_lenc(s, 0);
            s->set_wb_mode(s, 3);
            s->set_ae_level(s, 0);
            s->set_gainceiling(s, (gainceiling_t)3);
            s->set_contrast(s, 2);
            s->set_saturation(s, 2);
            // Punchy cross-processed Lomo without green midtone bias
            s->set_reg(s, 0x0C7, 0x40, 0x40);  // Enable manual AWB gain lock
            s->set_reg(s, 0x0CC, 0xFF, 0x62);  // Red gain   (neutral 0x5E)
            s->set_reg(s, 0x0CD, 0xFF, 0x3E);  // Green gain (neutral 0x41)
            s->set_reg(s, 0x0CE, 0xFF, 0x66);  // Blue gain  (neutral 0x54)
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
            return "NIGHT 5X";
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
