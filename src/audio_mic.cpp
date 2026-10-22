#include "audio_mic.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include <math.h>

static const char* TAG = "AUDIO_MIC";

bool AudioMic::_initialized = false;
bool AudioMic::_clapEnabled = true;   // Enabled by default
uint16_t AudioMic::_threshold = 160;  // Calibrated clap threshold (16% full-scale at 3x gain)
uint8_t AudioMic::_gain = 3;          // 3x digital gain for MEMS PDM microphone
uint16_t AudioMic::_currentLevel = 0;
uint16_t AudioMic::_peakHoldLevel = 0;
uint32_t AudioMic::_lastPeakTime = 0;
bool AudioMic::_clapDetected = false;
uint32_t AudioMic::_lastClapTime = 0;
bool AudioMic::_countdownActive = false;
uint32_t AudioMic::_countdownRemainingMs = 0;
float AudioMic::_prevSample = 0.0f;
float AudioMic::_prevFilter = 0.0f;

#define I2S_PORT I2S_NUM_0

bool AudioMic::init() {
    ESP_LOGI(TAG, "Initializing I2S PDM for XIAO Sense Mic on CLK:%d DATA:%d", MIC_PDM_CLK_PIN, MIC_PDM_DATA_PIN);

    i2s_config_t i2s_config = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
                               .sample_rate = 16000,
                               .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
                               .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
                               .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                               .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                               .dma_buf_count = 4,
                               .dma_buf_len = 256,
                               .use_apll = false,
                               .tx_desc_auto_clear = false,
                               .fixed_mclk = 0};

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = MIC_PDM_CLK_PIN,  // GPIO 42
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_PDM_DATA_PIN  // GPIO 41
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: 0x%x", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: 0x%x", err);
        return false;
    }

    _initialized = true;
    ESP_LOGI(TAG, "I2S PDM microphone initialized successfully. Default threshold: %d, Gain: %dx", _threshold, _gain);
    return true;
}

void AudioMic::update(uint32_t now) {
    if (!_initialized) return;
    if (now == 0) now = millis();

    int16_t samples[256];
    size_t bytes_read = 0;
    int32_t max_peak = 0;

    // Drain I2S DMA buffer (up to 8 blocks of 256 samples = up to 2048 samples / 128 ms audio).
    // Prevents sample loss between web server loop iterations.
    for (int iter = 0; iter < 8; iter++) {
        esp_err_t res = i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read, 0);
        if (res != ESP_OK || bytes_read == 0) break;

        size_t num_samples = bytes_read / sizeof(int16_t);
        for (size_t i = 0; i < num_samples; i++) {
            float in_sample = (float)samples[i];

            // 1-pole digital high-pass filter:
            // Eliminates DC offset and low-frequency rumble < 250 Hz while preserving sharp clap attacks
            float filtered = 0.93f * (_prevFilter + in_sample - _prevSample);
            _prevSample = in_sample;
            _prevFilter = filtered;

            int32_t val = (int32_t)fabsf(filtered);

            // Digital gain amplification
            val *= _gain;
            if (val > 32767) val = 32767;

            if (val > max_peak) {
                max_peak = val;
            }
        }
    }

    if (max_peak > 0) {
        _currentLevel = (uint16_t)((max_peak * 1000) / 32767);
    } else {
        // Smooth decay for instantaneous level
        _currentLevel = (_currentLevel * 7) / 10;
    }

    // Peak Hold: holds peak value for 350 ms, then decays smoothly.
    // Ensures HTTP polling (/audio_level) reliably captures transient clap spikes.
    if (_currentLevel > _peakHoldLevel) {
        _peakHoldLevel = _currentLevel;
        _lastPeakTime = now;
    } else if (now - _lastPeakTime > 350) {
        _peakHoldLevel = (_peakHoldLevel * 85) / 100;
        if (_peakHoldLevel < _currentLevel) {
            _peakHoldLevel = _currentLevel;
        }
    }

    // Clap impulse detection
    if (_clapEnabled && (_currentLevel >= _threshold)) {
        // Anti-retrigger lockout (2.5 second cooldown)
        if (now - _lastClapTime >= 2500) {
            _clapDetected = true;
            _lastClapTime = now;
            ESP_LOGI(TAG, "CLAP DETECTED! Instant: %d, PeakHold: %d, Threshold: %d", _currentLevel, _peakHoldLevel,
                     _threshold);
        }
    }
}

uint16_t AudioMic::getLevel() {
    return _currentLevel;
}

uint16_t AudioMic::getPeakLevel() {
    return _peakHoldLevel;
}

bool AudioMic::checkClapDetected() {
    if (_clapDetected) {
        _clapDetected = false;
        return true;
    }
    return false;
}

void AudioMic::setThreshold(uint16_t threshold) {
    _threshold = threshold;
    ESP_LOGI(TAG, "Clap threshold changed to: %d", _threshold);
}

uint16_t AudioMic::getThreshold() {
    return _threshold;
}

void AudioMic::setGain(uint8_t gain) {
    if (gain >= 1 && gain <= 10) {
        _gain = gain;
        ESP_LOGI(TAG, "Microphone gain changed to: %dx", _gain);
    }
}

uint8_t AudioMic::getGain() {
    return _gain;
}

void AudioMic::setClapTriggerEnabled(bool enabled) {
    _clapEnabled = enabled;
}

bool AudioMic::isClapTriggerEnabled() {
    return _clapEnabled;
}

void AudioMic::setCountdownStatus(bool active, uint32_t remainingMs) {
    _countdownActive = active;
    _countdownRemainingMs = remainingMs;
}

bool AudioMic::isCountdownActive() {
    return _countdownActive;
}

uint32_t AudioMic::getCountdownRemainingMs() {
    return _countdownRemainingMs;
}
