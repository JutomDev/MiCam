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

// 1. Cheerful "Mii-ii!" greeting (Two-tone Nintendo chime)
static const Note MELODY_MII_GREETING[] = {
    {880, 80},   // A5
    {0, 25},     // Pause
    {1175, 140}  // D6
};

// 2. Full Nintendo Wii Mii Channel melody
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

// 3. Mechanical camera shutter click simulation
static const Note MELODY_SHUTTER[] = {{2200, 15}, {0, 10}, {850, 20}};

// 4. Shutter countdown tick
static const Note MELODY_COUNTDOWN_TICK[] = {{1200, 45}};

// 5. Final high-pitch trigger cue
static const Note MELODY_COUNTDOWN_FINAL[] = {{2400, 120}};

// 6. Photo saved success confirmation chime
static const Note MELODY_SAVED[] = {
    {1046, 60},  // C6
    {1318, 60},  // E6
    {1568, 120}  // G6
};

bool Buzzer::_initialized = false;
SoundEffect Buzzer::_currentSfx = SFX_NONE;
size_t Buzzer::_noteIndex = 0;
uint32_t Buzzer::_noteStartTime = 0;
bool Buzzer::_isPlaying = false;

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
    } else {
        mute();
    }
#endif
}

void Buzzer::mute() {
#if USE_BUZZER
    if (!_initialized) return;
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
#endif
}

void Buzzer::play(SoundEffect sfx) {
#if USE_BUZZER
    if (!_initialized) return;
    _currentSfx = sfx;
    _noteIndex = 0;
    _noteStartTime = millis();
    _isPlaying = true;

    // Immediately play first note
    update(_noteStartTime);
#endif
}

void Buzzer::playMiiGreeting() {
    play(SFX_MII_GREETING);
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

void Buzzer::stop() {
    _isPlaying = false;
    _currentSfx = SFX_NONE;
    mute();
}

void Buzzer::update(uint32_t now) {
#if USE_BUZZER
    if (!_initialized || !_isPlaying) return;
    if (now == 0) now = millis();

    const Note* playlist = nullptr;
    size_t totalNotes = 0;

    switch (_currentSfx) {
        case SFX_MII_GREETING:
            playlist = MELODY_MII_GREETING;
            totalNotes = sizeof(MELODY_MII_GREETING) / sizeof(Note);
            break;
        case SFX_MII_THEME:
            playlist = MELODY_MII_THEME;
            totalNotes = sizeof(MELODY_MII_THEME) / sizeof(Note);
            break;
        case SFX_SHUTTER_CLICK:
            playlist = MELODY_SHUTTER;
            totalNotes = sizeof(MELODY_SHUTTER) / sizeof(Note);
            break;
        case SFX_COUNTDOWN_TICK:
            playlist = MELODY_COUNTDOWN_TICK;
            totalNotes = sizeof(MELODY_COUNTDOWN_TICK) / sizeof(Note);
            break;
        case SFX_COUNTDOWN_FINAL:
            playlist = MELODY_COUNTDOWN_FINAL;
            totalNotes = sizeof(MELODY_COUNTDOWN_FINAL) / sizeof(Note);
            break;
        case SFX_SAVED_SUCCESS:
            playlist = MELODY_SAVED;
            totalNotes = sizeof(MELODY_SAVED) / sizeof(Note);
            break;
        default:
            stop();
            return;
    }

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
        setTone(playlist[_noteIndex].freq);
    } else {
        // Ensure current tone is active
        setTone(playlist[_noteIndex].freq);
    }
#endif
}
