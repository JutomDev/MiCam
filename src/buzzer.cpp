#include "buzzer.h"
#include "esp_log.h"

static const char* TAG = "BUZZER";

#define BUZZER_LEDC_CHANNEL 7
#define BUZZER_LEDC_RES 8

struct Note {
    uint16_t freq;      // Hz (0 = cisza / pauza)
    uint16_t duration;  // ms
};

// ============================================================================
// CHIPTUNE MELODY NOTE TABLES
// ============================================================================

// 1. Cheerful "Mii-ii!" greeting on startup (rising vocal-like "Mii!" + Nintendo chime)
static const Note MELODY_MII_GREETING[] = {
    {659, 55},   // E5 ("M-")
    {784, 55},   // G5 ("-i-")
    {988, 70},   // B5 ("-i")
    {0, 30},     // Short breath
    {880, 75},   // A5 ("Mii-")
    {1175, 160}  // D6 ("-ii!")
};

// 2. Cute Cat "Mii!" vocalization for photo capture (smooth pitch-bend meow "M-i-i-i!")
static const Note MELODY_CAT_MII[] = {
    {2200, 12},  // Mechanical shutter click start
    {0, 15},     // Tiny pause
    {740, 35},   // F#5 ("M-")
    {880, 40},   // A5  ("-i-")
    {1047, 45},  // C6  ("-i-")
    {1319, 65},  // E6  ("-ii-")
    {1480, 80},  // F#6 (peak "Mii!")
    {1319, 55},  // E6  (gentle tail)
    {1175, 70}   // D6  (purr finish)
};

// 3. Full Nintendo Wii Mii Channel melody
static const Note MELODY_MII_THEME[] = {
    {370, 180},  // F#4
    {440, 180},  // A4
    {554, 240},  // C#5
    {0, 40},     // Pause
    {440, 180},  // A4
    {370, 180},  // F#4
    {294, 200},  // D4
    {0, 60},     // Pause
    {294, 120},  // D4
    {0, 40},     // Pause
    {294, 180}   // D4
};

// 4. Mechanical camera shutter click simulation
static const Note MELODY_SHUTTER[] = {{2200, 15}, {0, 10}, {850, 20}};

// 5. Shutter countdown tick
static const Note MELODY_COUNTDOWN_TICK[] = {{1200, 45}};

// 6. Final high-pitch trigger cue
static const Note MELODY_COUNTDOWN_FINAL[] = {{2400, 120}};

// 7. Photo saved success confirmation chime
static const Note MELODY_SAVED[] = {
    {1046, 55},  // C6
    {1318, 55},  // E6
    {1568, 110}  // G6
};

// 8. Crisp UI navigation tick
static const Note MELODY_UI_TICK[] = {{1400, 8}};

bool Buzzer::_initialized = false;
bool Buzzer::_muted = true;  // Speaker muted by default at boot
SoundEffect Buzzer::_currentSfx = SFX_NONE;
size_t Buzzer::_noteIndex = 0;
uint32_t Buzzer::_noteStartTime = 0;
bool Buzzer::_isPlaying = false;

static const Note* getPlaylist(SoundEffect sfx, size_t& totalNotes) {
    switch (sfx) {
        case SFX_MII_GREETING:
            totalNotes = sizeof(MELODY_MII_GREETING) / sizeof(Note);
            return MELODY_MII_GREETING;
        case SFX_CAT_MII:
            totalNotes = sizeof(MELODY_CAT_MII) / sizeof(Note);
            return MELODY_CAT_MII;
        case SFX_MII_THEME:
            totalNotes = sizeof(MELODY_MII_THEME) / sizeof(Note);
            return MELODY_MII_THEME;
        case SFX_SHUTTER_CLICK:
            totalNotes = sizeof(MELODY_SHUTTER) / sizeof(Note);
            return MELODY_SHUTTER;
        case SFX_COUNTDOWN_TICK:
            totalNotes = sizeof(MELODY_COUNTDOWN_TICK) / sizeof(Note);
            return MELODY_COUNTDOWN_TICK;
        case SFX_COUNTDOWN_FINAL:
            totalNotes = sizeof(MELODY_COUNTDOWN_FINAL) / sizeof(Note);
            return MELODY_COUNTDOWN_FINAL;
        case SFX_SAVED_SUCCESS:
            totalNotes = sizeof(MELODY_SAVED) / sizeof(Note);
            return MELODY_SAVED;
        case SFX_UI_TICK:
            totalNotes = sizeof(MELODY_UI_TICK) / sizeof(Note);
            return MELODY_UI_TICK;
        default:
            totalNotes = 0;
            return nullptr;
    }
}

void Buzzer::init() {
#if USE_BUZZER
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    ledcSetup(BUZZER_LEDC_CHANNEL, 2000, BUZZER_LEDC_RES);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);

    _initialized = true;
    ESP_LOGI(TAG, "Piezo Buzzer initialized on GPIO %d (LEDC ch %d)", BUZZER_PIN, BUZZER_LEDC_CHANNEL);
#endif
}

void Buzzer::setTone(uint32_t freq) {
#if USE_BUZZER
    if (!_initialized) return;
    if (freq > 0) {
        ledcWriteTone(BUZZER_LEDC_CHANNEL, freq);
        ledcWrite(BUZZER_LEDC_CHANNEL, 128);  // 50% duty cycle for clean, loud square wave
    } else {
        mute();
    }
#endif
}

void Buzzer::mute() {
#if USE_BUZZER
    if (!_initialized) return;
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
#endif
}

void Buzzer::play(SoundEffect sfx) {
#if USE_BUZZER
    if (!_initialized) return;
    if (_muted) {
        stop();
        return;
    }
    size_t totalNotes = 0;
    const Note* playlist = getPlaylist(sfx, totalNotes);
    if (!playlist || totalNotes == 0) {
        stop();
        return;
    }

    _currentSfx = sfx;
    _noteIndex = 0;
    _noteStartTime = millis();
    _isPlaying = true;

    // Set first note hardware frequency once (without re-triggering on every loop tick)
    setTone(playlist[0].freq);
#endif
}

void Buzzer::playMiiGreeting() {
    play(SFX_MII_GREETING);
}
void Buzzer::playCatMii() {
    play(SFX_CAT_MII);
}
void Buzzer::playMiiTheme() {
    play(SFX_MII_THEME);
}
void Buzzer::playShutterClick() {
    play(SFX_SHUTTER_CLICK);
}
void Buzzer::playCountdownTick() {
    play(SFX_COUNTDOWN_TICK);
}
void Buzzer::playCountdownFinal() {
    play(SFX_COUNTDOWN_FINAL);
}
void Buzzer::playSavedSuccess() {
    play(SFX_SAVED_SUCCESS);
}
void Buzzer::playUiTick() {
    play(SFX_UI_TICK);
}

bool Buzzer::isPlaying() {
    return _isPlaying;
}

void Buzzer::setMuted(bool muted) {
    _muted = muted;
    if (_muted) {
        stop();
    }
    ESP_LOGI(TAG, "Speaker mute state: %s", _muted ? "MUTED" : "UNMUTED");
}

bool Buzzer::isMuted() {
    return _muted;
}

void Buzzer::toggleMute() {
    setMuted(!_muted);
}

void Buzzer::stop() {
    _isPlaying = false;
    _currentSfx = SFX_NONE;
    mute();
}

void Buzzer::delayMs(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        update(millis());
        delay(2);
    }
}

void Buzzer::update(uint32_t now) {
#if USE_BUZZER
    if (!_initialized || !_isPlaying) return;
    if (now == 0) now = millis();

    size_t totalNotes = 0;
    const Note* playlist = getPlaylist(_currentSfx, totalNotes);

    if (!playlist || _noteIndex >= totalNotes) {
        stop();
        return;
    }

    uint32_t elapsed = now - _noteStartTime;
    if (elapsed >= playlist[_noteIndex].duration) {
        _noteIndex++;
        if (_noteIndex >= totalNotes) {
            stop();
            return;
        }
        _noteStartTime = now;
        // Only call setTone() when advancing to the next note!
        setTone(playlist[_noteIndex].freq);
    }
#endif
}
