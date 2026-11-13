#pragma once

#include <Arduino.h>
#include "config.h"

// ============================================================================
// PIEZO BUZZER SOUND SUBSYSTEM (Pin D7 / GPIO 44)
// ============================================================================
// Fully non-blocking chiptune tone player and sound effects engine

enum SoundEffect {
    SFX_NONE = 0,
    SFX_MII_GREETING,     // Iconic Nintendo "Mii-ii!" greeting on boot
    SFX_CAT_MII,          // Cute cat-like "Mii!" sound when taking a photo
    SFX_MII_THEME,        // Full Nintendo Wii Mii Channel melody
    SFX_SHUTTER_CLICK,    // Mechanical camera shutter click simulation
    SFX_COUNTDOWN_TICK,   // Short countdown tick (1000 Hz)
    SFX_COUNTDOWN_FINAL,  // High-pitch shutter trigger cue (2200 Hz)
    SFX_SAVED_SUCCESS,    // Confirmation chime for photo saved to SD card
    SFX_UI_TICK           // Subtle UI navigation tick
};

class Buzzer {
public:
    static void init();
    static void update(uint32_t now = 0);
    static void delayMs(uint32_t ms);

    // Audio effect triggers
    static void play(SoundEffect sfx);
    static void playMiiGreeting();
    static void playCatMii();
    static void playMiiTheme();
    static void playShutterClick();
    static void playCountdownTick();
    static void playCountdownFinal();
    static void playSavedSuccess();
    static void playUiTick();

    static bool isPlaying();

    // Global speaker mute control (muted by default at boot)
    static void setMuted(bool muted);
    static bool isMuted();
    static void toggleMute();

    // Stop active playback
    static void stop();

private:
    static bool _initialized;
    static bool _muted;
    static SoundEffect _currentSfx;
    static size_t _noteIndex;
    static uint32_t _noteStartTime;
    static bool _isPlaying;

    static void setTone(uint32_t freq);
    static void mute();
};
