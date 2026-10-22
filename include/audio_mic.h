#pragma once

#include <Arduino.h>
#include "config.h"

class AudioMic {
public:
    static bool init();
    static void update(uint32_t now = 0);
    static uint16_t getLevel();       // Current audio level (instantaneous RMS) 0-1000
    static uint16_t getPeakLevel();   // Peak-hold audio level (for Web UI VU meter) 0-1000
    static bool checkClapDetected();  // Returns true once when a clap impulse is detected
    static void setThreshold(uint16_t threshold);
    static uint16_t getThreshold();
    static void setGain(uint8_t gain);  // Digital gain multiplier (1x, 2x, 3x...)
    static uint8_t getGain();
    static void setClapTriggerEnabled(bool enabled);
    static bool isClapTriggerEnabled();
    static void setCountdownStatus(bool active, uint32_t remainingMs = 0);
    static bool isCountdownActive();
    static uint32_t getCountdownRemainingMs();

private:
    static bool _initialized;
    static bool _clapEnabled;
    static uint16_t _threshold;
    static uint8_t _gain;
    static uint16_t _currentLevel;
    static uint16_t _peakHoldLevel;
    static uint32_t _lastPeakTime;
    static bool _clapDetected;
    static uint32_t _lastClapTime;
    static bool _countdownActive;
    static uint32_t _countdownRemainingMs;
    static float _prevSample;
    static float _prevFilter;
};
