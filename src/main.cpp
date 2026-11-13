#include <Arduino.h>
#include "config.h"
#include "camera_driver.h"
#include "display_ui.h"
#include "sd_storage.h"
#include "wifi_share.h"
#include "audio_mic.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_system.h"

static const char* TAG = "MAIN_APP";

// Acoustic clap trigger state variables
static bool clapTriggerPending = false;
static uint32_t clapCountdownStart = 0;

// ============================================================================
// NON-BLOCKING BUTTON ENGINE (Debounce & Long Press based on millis())
// ============================================================================
struct Button {
    int8_t pin;
    bool lastState;
    bool isPressed;
    uint32_t pressStartTime;
    bool longPressFired;
    bool shortClicked;
    bool longClicked;

    void init(int8_t p) {
        pin = p;
        lastState = HIGH;
        isPressed = false;
        pressStartTime = 0;
        longPressFired = false;
        shortClicked = false;
        longClicked = false;
        if (pin >= 0) {
            pinMode(pin, INPUT_PULLUP);
        }
    }

    void update(uint32_t now) {
        shortClicked = false;
        longClicked = false;
        if (pin < 0) return;

        bool currentPinState = digitalRead(pin);

        // State change: HIGH -> LOW (Press to GND)
        if (lastState == HIGH && currentPinState == LOW) {
            isPressed = true;
            pressStartTime = now;
            longPressFired = false;
        }
        // Continuous button hold
        else if (currentPinState == LOW && isPressed) {
            if (!longPressFired && (now - pressStartTime >= LONG_PRESS_TIME_MS)) {
                longClicked = true;
                longPressFired = true;
            }
        }
        // Release: LOW -> HIGH
        else if (lastState == LOW && currentPinState == HIGH) {
            if (isPressed) {
                uint32_t duration = now - pressStartTime;
                if (!longPressFired && (duration >= DEBOUNCE_TIME_MS)) {
                    shortClicked = true;
                }
            }
            isPressed = false;
        }

        lastState = currentPinState;
    }
};

enum JoyDirection { JOY_NONE = 0, JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT };

struct AnalogJoystick {
    int8_t pinX;
    int8_t pinY;
    uint32_t lastActionTime;
    uint32_t centerSinceTime;
    uint32_t pendingSince;
    JoyDirection latchedDir;
    JoyDirection pendingDir;
    uint8_t repeatCount;
    bool waitForNeutral;

    void init(int8_t px, int8_t py) {
        pinX = px;
        pinY = py;
        lastActionTime = 0;
        centerSinceTime = 0;
        pendingSince = 0;
        latchedDir = JOY_NONE;
        pendingDir = JOY_NONE;
        repeatCount = 0;
        waitForNeutral = false;
        if (pinX >= 0) pinMode(pinX, INPUT);
        if (pinY >= 0) pinMode(pinY, INPUT);
    }

    void readAveraged(int& outX, int& outY) {
        int sumX = 0, sumY = 0;
        for (int i = 0; i < 4; i++) {
            sumX += analogRead(pinX);
            sumY += analogRead(pinY);
        }
        outX = sumX >> 2;
        outY = sumY >> 2;
    }

    // Require stick to return to center before registering next menu action (used on state transitions)
    void waitRelease() {
        waitForNeutral = true;
        latchedDir = JOY_NONE;
        pendingDir = JOY_NONE;
        pendingSince = 0;
        repeatCount = 0;
    }

    // Instantaneous direction for real-time arcade mini-game steering
    JoyDirection getRawDirection() {
#if USE_ANALOG_JOYSTICK
        if (pinX < 0 || pinY < 0) return JOY_NONE;
        int xVal = 2048, yVal = 2048;
        readAveraged(xVal, yVal);

        int dx = xVal - 2048;
        int dy = yVal - 2048;
        const int DEADZONE = 950;

        if (abs(dx) < DEADZONE && abs(dy) < DEADZONE) return JOY_NONE;
        if (abs(dy) >= abs(dx)) {
            return (dy < 0) ? JOY_UP : JOY_DOWN;
        } else {
            // Invert X axis so tilting joystick left turns left, tilting right turns right
            return (dx < 0) ? JOY_RIGHT : JOY_LEFT;
        }
#else
        return JOY_NONE;
#endif
    }

    // Debounced & Schmitt-trigger hysteresis direction for crisp 1-step-per-push UI navigation
    JoyDirection getDirection(uint32_t now) {
#if USE_ANALOG_JOYSTICK
        if (pinX < 0 || pinY < 0) return JOY_NONE;
        int xVal = 2048, yVal = 2048;
        readAveraged(xVal, yVal);

        // Schmitt trigger thresholds (12-bit ADC: 0..4095, center ~2048)
        const int TRIG_LOW = 800;
        const int TRIG_HIGH = 3300;
        const int REL_LOW = 1400;
        const int REL_HIGH = 2700;

        bool inNeutral = (xVal > REL_LOW && xVal < REL_HIGH && yVal > REL_LOW && yVal < REL_HIGH);

        if (inNeutral) {
            pendingDir = JOY_NONE;
            pendingSince = 0;
            if (centerSinceTime == 0) {
                centerSinceTime = now;
            } else if (now - centerSinceTime >= 65) {
                // Stick has been stably in neutral for 65ms -> reset latch
                latchedDir = JOY_NONE;
                repeatCount = 0;
                waitForNeutral = false;
            }
            return JOY_NONE;
        }

        // Stick moved outside neutral zone
        centerSinceTime = 0;
        if (waitForNeutral) {
            return JOY_NONE;
        }

        JoyDirection candidate = JOY_NONE;
        int dx = xVal - 2048;
        int dy = yVal - 2048;

        if (abs(dy) >= abs(dx)) {
            if (yVal < TRIG_LOW)
                candidate = JOY_UP;
            else if (yVal > TRIG_HIGH)
                candidate = JOY_DOWN;
        } else {
            // Invert X axis so stick movement matches screen direction
            if (xVal < TRIG_LOW)
                candidate = JOY_RIGHT;
            else if (xVal > TRIG_HIGH)
                candidate = JOY_LEFT;
        }

        if (candidate == JOY_NONE) {
            pendingDir = JOY_NONE;
            pendingSince = 0;
            return JOY_NONE;
        }

        // Require >= 20ms continuous deflection to reject Wi-Fi/RF ADC glitches
        if (latchedDir == JOY_NONE) {
            if (pendingDir != candidate) {
                pendingDir = candidate;
                pendingSince = now;
                return JOY_NONE;
            }
            if (now - pendingSince < 20) {
                return JOY_NONE;
            }
            latchedDir = candidate;
            pendingDir = JOY_NONE;
            repeatCount = 0;
            lastActionTime = now;
            return candidate;
        }

        // Held down continuously: wait 550ms for first repeat, then 350ms per step
        uint32_t repeatDelay = (repeatCount == 0) ? 550 : 350;
        if (candidate == latchedDir && (now - lastActionTime >= repeatDelay)) {
            if (repeatCount < 250) repeatCount++;
            lastActionTime = now;
            return candidate;
        }
#endif
        return JOY_NONE;
    }
};

static Button btnShutter;
static Button btnNav;
static AnalogJoystick joystick;

// ============================================================================
// SYSTEM STATE VARIABLES
// ============================================================================
static CameraState currentState = STATE_LIVE_VIEW;
static uint8_t menuSelectedIndex = 0;
static int galleryPhotoIndex = 0;
static uint32_t lastFpsTime = 0;
static uint32_t frameCount = 0;

// Forward declarations
void handleLiveView(uint32_t now);
void handleCapture();
void handleTimelapse(uint32_t now);
void handleGallery();
void handleMenu();
void handleWifiServer(uint32_t now);
void initMiniGame();
void handleMiniGame(uint32_t now);
void enterState(CameraState newState);

// ============================================================================
// FSM STATE TRANSITION HELPERS
// ============================================================================
void enterState(CameraState newState) {
    if (currentState == newState) return;

    ESP_LOGI(TAG, "FSM Transition: %d -> %d", (int)currentState, (int)newState);

    // Prevent joystick held during transition from firing immediately in the new screen
    joystick.waitRelease();
    btnShutter.shortClicked = false;
    btnShutter.longClicked = false;
    btnNav.shortClicked = false;
    btnNav.longClicked = false;

    // Teardown previous state
    if (currentState == STATE_WIFI_SERVER) {
        WifiShare::stop();
        CameraDriver::setModePreview();
    }

    currentState = newState;

    switch (currentState) {
        case STATE_LIVE_VIEW:
            DisplayUI::getLcd().fillScreen(0x0000);
            DisplayUI::invalidateHud();
            CameraDriver::setModePreview();
            break;

        case STATE_GALLERY: {
            Buzzer::stop();
            SdStorage::refreshList();
            std::vector<String> list = SdStorage::getPhotoList();
            if (list.empty()) {
                DisplayUI::drawGalleryEmpty();
                galleryPhotoIndex = 0;
            } else {
                galleryPhotoIndex = list.size() - 1;  // Start with newest capture
                DisplayUI::drawGalleryPhoto(list[galleryPhotoIndex].c_str(), galleryPhotoIndex + 1, list.size());
            }
            break;
        }

        case STATE_MENU:
            DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), true);
            break;

        case STATE_WIFI_SERVER:
            DisplayUI::showMessage("STARTING WI-FI", "Launching AP...", 150);
            if (WifiShare::start()) {
                DisplayUI::drawWifiServerScreen(WIFI_AP_SSID, WifiShare::getIP().c_str(), SdStorage::getPhotoCount());
            } else {
                DisplayUI::showMessage("WI-FI ERROR", "Failed to start AP", 1000);
                enterState(STATE_MENU);
            }
            break;

        case STATE_MINIGAME:
            initMiniGame();
            break;

        case STATE_CAPTURE:
        case STATE_TIMELAPSE:
        case STATE_BURST_STACK:
        case STATE_RECORD_VIDEO:
            break;
    }
}

// ============================================================================
// INDIVIDUAL STATE HANDLERS
// ============================================================================

void handleLiveView(uint32_t now) {
    JoyDirection joy = joystick.getDirection(now);

    // 1. Handle user inputs
    if (btnShutter.shortClicked) {
        enterState(STATE_CAPTURE);
        return;
    }

    if (btnNav.shortClicked || joy == JOY_RIGHT || joy == JOY_LEFT) {
        // Fast cycling through camera presets on the fly
        Buzzer::playUiTick();
        CameraDriver::nextPreset();
        DisplayUI::invalidateHud();
    }

    if (btnNav.longClicked || btnShutter.longClicked || joy == JOY_UP || joy == JOY_DOWN) {
        // Long-press or joystick UP/DOWN opens main settings menu
        Buzzer::playUiTick();
        enterState(STATE_MENU);
        return;
    }

    // 2. Process digital PDM microphone and acoustic clap trigger
    AudioMic::update(now);
    if (AudioMic::checkClapDetected() && !clapTriggerPending) {
        ESP_LOGI(TAG, "Clap sound detected! Starting 2-second countdown...");
        clapTriggerPending = true;
        clapCountdownStart = now;
        AudioMic::setCountdownStatus(true, 2000);
        Buzzer::playCountdownTick();
    }

    if (clapTriggerPending) {
        uint32_t elapsed = now - clapCountdownStart;
        uint32_t rem = (elapsed < 2000) ? (2000 - elapsed) : 0;
        AudioMic::setCountdownStatus(true, rem);

#if USE_TALLY_LED
        bool blink = (elapsed < 1200) ? ((elapsed / 250) % 2 == 0) : ((elapsed / 100) % 2 == 0);
        digitalWrite(LED_TALLY_PIN, blink ? HIGH : LOW);
#endif
        if (elapsed >= 2000) {
            clapTriggerPending = false;
            AudioMic::setCountdownStatus(false, 0);
#if USE_TALLY_LED
            digitalWrite(LED_TALLY_PIN, LOW);
#endif
            handleCapture();
            return;
        }
    }

    // 3. Acquire and push preview frame (RGB565 240x240)
    camera_fb_t* fb = CameraDriver::getFrame();
    if (fb) {
        DisplayUI::drawLiveView(fb->buf, fb->width, fb->height,
                                CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                SdStorage::getPhotoCount(), SdStorage::isAvailable());
        CameraDriver::returnFrame(fb);

        frameCount++;
        if (now - lastFpsTime >= 2000) {
            float fps = (frameCount * 1000.0f) / (now - lastFpsTime);
            ESP_LOGI(TAG, "Live View: %.1f FPS | Free Heap: %d | Free PSRAM: %d", fps, ESP.getFreeHeap(),
                     ESP.getFreePsram());
            frameCount = 0;
            lastFpsTime = now;
        }
    }
}

void handleCapture() {
    ESP_LOGI(TAG, "Capture initiated!");

    // Play cute cat "Mii!" sound when taking a photo!
    Buzzer::playCatMii();

#if USE_TALLY_LED
    digitalWrite(LED_TALLY_PIN, HIGH);
#endif

    // 1. White screen flash animation (keeps updating Buzzer during flash)
    DisplayUI::drawFlashAnimation();

    // 2. Capture and encode high-quality JPEG using static PSRAM buffer (zero malloc churn)
    DisplayUI::drawSavingProgress("ENCODING JPEG...");
    const uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    if (!CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 92)) {
        ESP_LOGE(TAG, "Capture failed!");
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        DisplayUI::showMessage("CAM ERROR", "Capture failed", 700);
        enterState(STATE_LIVE_VIEW);
        return;
    }

    Buzzer::update();

    // 3. Save to SD card file with embedded EXIF metadata
    char savedFilename[32];
    DisplayUI::drawSavingProgress("WRITING TO SD...");
    bool saved = SdStorage::savePhoto(jpgBuf, jpgLen, savedFilename, sizeof(savedFilename));

    Buzzer::update();

#if USE_TALLY_LED
    digitalWrite(LED_TALLY_PIN, LOW);
#endif

    if (saved) {
        ESP_LOGI(TAG, "Photo successfully saved: %s", savedFilename);
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, HIGH);
        Buzzer::delayMs(45);
        digitalWrite(LED_TALLY_PIN, LOW);
        Buzzer::delayMs(45);
        digitalWrite(LED_TALLY_PIN, HIGH);
        Buzzer::delayMs(45);
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        DisplayUI::showMessage("MII! SAVED", savedFilename, 450);
    } else {
        ESP_LOGE(TAG, "Failed to save photo to SD!");
        DisplayUI::showMessage("NO SD CARD", "Insert FAT32 SD", 900);
    }

    // 4. Return to Live View
    enterState(STATE_LIVE_VIEW);
}

static uint32_t lastTimelapseShot = 0;
static uint16_t timelapseCount = 0;
const uint32_t TIMELAPSE_INTERVAL_MS = 5000;

void handleTimelapse(uint32_t now) {
    if (btnNav.shortClicked || btnShutter.shortClicked || btnNav.longClicked) {
        ESP_LOGI(TAG, "Exiting Timelapse mode");
        enterState(STATE_LIVE_VIEW);
        return;
    }

    uint32_t elapsed = now - lastTimelapseShot;
    if (elapsed >= TIMELAPSE_INTERVAL_MS) {
        lastTimelapseShot = now;
        timelapseCount++;
        handleCapture();
    }
}

void handleGallery() {
    std::vector<String> list = SdStorage::getPhotoList();
    uint32_t now = millis();
    JoyDirection joy = joystick.getDirection(now);

    if (list.empty()) {
        if (btnNav.shortClicked || btnShutter.shortClicked || joy != JOY_NONE) {
            Buzzer::playUiTick();
            enterState(STATE_MENU);
        }
        return;
    }

    // Hold SHUTTER (D0) deletes the current photo (e.g. corrupt or unwanted)
    if (btnShutter.longClicked) {
        String toDelete = list[galleryPhotoIndex];
        ESP_LOGI(TAG, "Deleting photo from gallery: %s", toDelete.c_str());
        Buzzer::playCountdownTick();
        DisplayUI::showMessage("DELETING...", toDelete.c_str(), 100);
        SdStorage::deletePhoto(toDelete.c_str());
        list = SdStorage::getPhotoList();
        if (list.empty()) {
            DisplayUI::drawGalleryEmpty();
            galleryPhotoIndex = 0;
        } else {
            if (galleryPhotoIndex >= (int)list.size()) {
                galleryPhotoIndex = list.size() - 1;
            }
            DisplayUI::drawGalleryPhoto(list[galleryPhotoIndex].c_str(), galleryPhotoIndex + 1, list.size());
        }
        return;
    }

    // Joystick LEFT - previous image
    if (btnNav.shortClicked || joy == JOY_LEFT) {
        Buzzer::playUiTick();
        if (galleryPhotoIndex > 0) {
            galleryPhotoIndex--;
        } else {
            galleryPhotoIndex = (int)list.size() - 1;
        }
        DisplayUI::drawGalleryPhoto(list[galleryPhotoIndex].c_str(), galleryPhotoIndex + 1, list.size());
    } else if (joy == JOY_RIGHT) {
        Buzzer::playUiTick();
        if (galleryPhotoIndex + 1 < (int)list.size()) {
            galleryPhotoIndex++;
        } else {
            galleryPhotoIndex = 0;
        }
        DisplayUI::drawGalleryPhoto(list[galleryPhotoIndex].c_str(), galleryPhotoIndex + 1, list.size());
    }

    // Short-press SHUTTER or joy UP/DOWN - return to Menu
    if (btnShutter.shortClicked || joy == JOY_UP || joy == JOY_DOWN) {
        Buzzer::playUiTick();
        enterState(STATE_MENU);
    }
}

void handleMenu() {
    uint32_t now = millis();
    JoyDirection joy = joystick.getDirection(now);
    const uint8_t MENU_ITEMS_COUNT = 7;

    // Downwards / Upwards navigation through menu options (partial redraw = zero flicker!)
    if (btnNav.shortClicked || joy == JOY_DOWN) {
        Buzzer::playUiTick();
        menuSelectedIndex = (menuSelectedIndex + 1) % MENU_ITEMS_COUNT;
        DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                            DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), false);
        return;
    } else if (joy == JOY_UP) {
        Buzzer::playUiTick();
        menuSelectedIndex = (menuSelectedIndex == 0) ? (MENU_ITEMS_COUNT - 1) : (menuSelectedIndex - 1);
        DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                            DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), false);
        return;
    }

    // Select highlighted item (SHUTTER or joystick RIGHT)
    if (btnShutter.shortClicked || joy == JOY_RIGHT) {
        switch (menuSelectedIndex) {
            case 0:  // Cycle image preset
                Buzzer::playUiTick();
                CameraDriver::nextPreset();
                DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                    DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), false);
                break;

            case 1:  // Cycle screen brightness (25% / 50% / 75% / 100%)
                Buzzer::playUiTick();
                DisplayUI::cycleBrightness();
                DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                    DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), false);
                break;

            case 2:  // Toggle speaker mute (default: MUTED)
                Buzzer::toggleMute();
                Buzzer::playUiTick();
                DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                    DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), false);
                break;

            case 3:  // Open SD photo gallery
                Buzzer::playUiTick();
                enterState(STATE_GALLERY);
                break;

            case 4:  // Launch SoftAP Wi-Fi file server
                Buzzer::playUiTick();
                enterState(STATE_WIFI_SERVER);
                break;

            case 5:  // Launch Retro Mini-Game: Mii-Snake!
                Buzzer::playUiTick();
                enterState(STATE_MINIGAME);
                break;

            case 6:  // Return to Live View
                Buzzer::playUiTick();
                enterState(STATE_LIVE_VIEW);
                break;
        }
        return;
    }

    // Long-press SHUTTER/NAV or joystick LEFT exits to camera
    if (btnNav.longClicked || btnShutter.longClicked || joy == JOY_LEFT) {
        Buzzer::playUiTick();
        enterState(STATE_LIVE_VIEW);
    }
}

// ============================================================================
// RETRO MINI-GAME: "MII-SNAKE" (Zero-flicker incremental rendering)
// ============================================================================
static const int SNAKE_COLS = 22;
static const int SNAKE_ROWS = 20;
static const int SNAKE_CELL = 10;
static const int SNAKE_OFFSET_X = 10;
static const int SNAKE_OFFSET_Y = 38;
static const int SNAKE_MAX_LEN = 128;

struct GridPt {
    int8_t x;
    int8_t y;
};

static GridPt snakeBody[SNAKE_MAX_LEN];
static int snakeLen = 3;
static int8_t snakeDx = 1;
static int8_t snakeDy = 0;
static int8_t nextSnakeDx = 1;
static int8_t nextSnakeDy = 0;
static GridPt snakeFood = {10, 10};
static uint16_t snakeScore = 0;
static uint16_t snakeHighScore = 0;
static uint32_t lastSnakeStep = 0;
static bool snakeGameOver = false;

static void drawSnakeCell(int gx, int gy, uint16_t color, bool isHead = false, bool isFood = false) {
    auto& lcd = DisplayUI::getLcd();
    int px = SNAKE_OFFSET_X + gx * SNAKE_CELL;
    int py = SNAKE_OFFSET_Y + gy * SNAKE_CELL;
    if (isFood) {
        lcd.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, 0x0000);
        lcd.fillRect(px + 2, py + 2, SNAKE_CELL - 4, SNAKE_CELL - 4, 0xF81F);  // Magenta Mii-Treat
        lcd.drawRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 0xFD20);
    } else if (isHead) {
        lcd.fillRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 0xFD20);  // Cat Amber Head
        // Cute cat eyes
        lcd.drawPixel(px + 3, py + 3, 0x0000);
        lcd.drawPixel(px + 6, py + 3, 0x0000);
    } else {
        lcd.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, 0x0000);
        if (color != 0x0000) {
            lcd.fillRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, color);
        }
    }
}

static void drawSnakeHeader() {
    auto& lcd = DisplayUI::getLcd();
    char buf[36];
    snprintf(buf, sizeof(buf), "MII-SNAKE  SCORE:%02d  HI:%02d", snakeScore, snakeHighScore);
    lcd.setTextSize(1);
    lcd.setTextColor(0xFD20, 0x10A2);
    lcd.drawCenterString(buf, 120, 12);
}

static void spawnSnakeFood() {
    for (int tries = 0; tries < 100; tries++) {
        int fx = random(0, SNAKE_COLS);
        int fy = random(0, SNAKE_ROWS);
        bool collision = false;
        for (int i = 0; i < snakeLen; i++) {
            if (snakeBody[i].x == fx && snakeBody[i].y == fy) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            snakeFood.x = fx;
            snakeFood.y = fy;
            drawSnakeCell(fx, fy, 0, false, true);
            return;
        }
    }
}

void initMiniGame() {
    auto& lcd = DisplayUI::getLcd();
    lcd.fillScreen(0x0821);

    // Header Bar
    lcd.fillRect(0, 0, TFT_WIDTH, 28, 0x10A2);
    lcd.drawFastHLine(0, 28, TFT_WIDTH, 0xFD20);

    // Playfield border (220x200 inside 240x280)
    lcd.fillRect(SNAKE_OFFSET_X - 2, SNAKE_OFFSET_Y - 2, SNAKE_COLS * SNAKE_CELL + 4, SNAKE_ROWS * SNAKE_CELL + 4,
                 0x0000);
    lcd.drawRect(SNAKE_OFFSET_X - 2, SNAKE_OFFSET_Y - 2, SNAKE_COLS * SNAKE_CELL + 4, SNAKE_ROWS * SNAKE_CELL + 4,
                 0xFD20);

    // Footer instructions
    lcd.setTextSize(1);
    lcd.setTextColor(0x07FF, 0x0821);
    lcd.drawCenterString("JOY: Steer | Click D0: Exit", 120, 252);

    snakeLen = 4;
    snakeDx = 1;
    snakeDy = 0;
    nextSnakeDx = 1;
    nextSnakeDy = 0;
    snakeScore = 0;
    snakeGameOver = false;

    for (int i = 0; i < snakeLen; i++) {
        snakeBody[i].x = 6 - i;
        snakeBody[i].y = 10;
        drawSnakeCell(snakeBody[i].x, snakeBody[i].y, 0x07E0, (i == 0));
    }

    drawSnakeHeader();
    spawnSnakeFood();
    Buzzer::playMiiGreeting();
    lastSnakeStep = millis();
}

void handleMiniGame(uint32_t now) {
    // Poll raw joystick continuously for responsive steering
    JoyDirection rawJoy = joystick.getRawDirection();
    if (rawJoy == JOY_UP && snakeDy == 0) {
        nextSnakeDx = 0;
        nextSnakeDy = -1;
    } else if (rawJoy == JOY_DOWN && snakeDy == 0) {
        nextSnakeDx = 0;
        nextSnakeDy = 1;
    } else if (rawJoy == JOY_LEFT && snakeDx == 0) {
        nextSnakeDx = -1;
        nextSnakeDy = 0;
    } else if (rawJoy == JOY_RIGHT && snakeDx == 0) {
        nextSnakeDx = 1;
        nextSnakeDy = 0;
    }

    if (snakeGameOver) {
        if (btnShutter.shortClicked) {
            // Restart game
            initMiniGame();
            return;
        }
        if (btnShutter.longClicked || rawJoy == JOY_LEFT) {
            Buzzer::playUiTick();
            enterState(STATE_MENU);
            return;
        }
        return;
    }

    // While playing, clicking Shutter exits back to Menu
    if (btnShutter.shortClicked || btnShutter.longClicked) {
        Buzzer::playUiTick();
        enterState(STATE_MENU);
        return;
    }

    uint32_t stepInterval = (snakeScore < 15) ? (165 - snakeScore * 5) : 90;
    if (now - lastSnakeStep < stepInterval) {
        return;
    }
    lastSnakeStep = now;

    snakeDx = nextSnakeDx;
    snakeDy = nextSnakeDy;

    int newX = snakeBody[0].x + snakeDx;
    int newY = snakeBody[0].y + snakeDy;

    // Wall wrap-around or collision? Classic wall collision:
    bool hit = (newX < 0 || newX >= SNAKE_COLS || newY < 0 || newY >= SNAKE_ROWS);
    if (!hit) {
        for (int i = 0; i < snakeLen - 1; i++) {
            if (snakeBody[i].x == newX && snakeBody[i].y == newY) {
                hit = true;
                break;
            }
        }
    }

    if (hit) {
        snakeGameOver = true;
        Buzzer::playCountdownTick();
        auto& lcd = DisplayUI::getLcd();
        lcd.fillRect(36, 105, 168, 70, 0x10A2);
        lcd.drawRect(36, 105, 168, 70, 0xF800);
        lcd.setTextSize(2);
        lcd.setTextColor(0xF800, 0x10A2);
        lcd.drawCenterString("GAME OVER", 120, 115);
        lcd.setTextSize(1);
        lcd.setTextColor(0xFFFF, 0x10A2);
        lcd.drawCenterString("D0: Retry | Hold D0: Exit", 120, 148);
        return;
    }

    bool ateFood = (newX == snakeFood.x && newY == snakeFood.y);
    GridPt oldTail = snakeBody[snakeLen - 1];

    // Shift body segments
    if (ateFood && snakeLen < SNAKE_MAX_LEN) {
        snakeLen++;
    } else {
        // Erase old tail pixel block
        drawSnakeCell(oldTail.x, oldTail.y, 0x0000);
    }

    for (int i = snakeLen - 1; i > 0; i--) {
        snakeBody[i] = snakeBody[i - 1];
    }
    snakeBody[0].x = newX;
    snakeBody[0].y = newY;

    // Redraw previous head as green body segment, and draw new cat head
    drawSnakeCell(snakeBody[1].x, snakeBody[1].y, 0x07E0, false);
    drawSnakeCell(snakeBody[0].x, snakeBody[0].y, 0xFD20, true);

    if (ateFood) {
        snakeScore++;
        if (snakeScore > snakeHighScore) {
            snakeHighScore = snakeScore;
        }
        Buzzer::playCatMii();
        drawSnakeHeader();
        spawnSnakeFood();
    }
}

void handleWifiServer(uint32_t now) {
    WifiShare::loop();

    // 1. Digital PDM mic acoustic clap trigger in Wi-Fi mode
    AudioMic::update(now);
    if (AudioMic::checkClapDetected() && !clapTriggerPending) {
        ESP_LOGI(TAG, "Clap detected in Wi-Fi mode! Starting 2-second countdown...");
        clapTriggerPending = true;
        clapCountdownStart = now;
        AudioMic::setCountdownStatus(true, 2000);
        Buzzer::playCountdownTick();
    }

    if (clapTriggerPending) {
        uint32_t elapsed = now - clapCountdownStart;
        uint32_t rem = (elapsed < 2000) ? (2000 - elapsed) : 0;
        AudioMic::setCountdownStatus(true, rem);

#if USE_TALLY_LED
        bool blink = (elapsed < 1200) ? ((elapsed / 250) % 2 == 0) : ((elapsed / 100) % 2 == 0);
        digitalWrite(LED_TALLY_PIN, blink ? HIGH : LOW);
#endif
        if (elapsed >= 2000) {
            clapTriggerPending = false;
            AudioMic::setCountdownStatus(false, 0);
#if USE_TALLY_LED
            digitalWrite(LED_TALLY_PIN, LOW);
#endif
            Buzzer::playCatMii();

            const uint8_t* jpgBuf = nullptr;
            size_t jpgLen = 0;
            if (CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 92)) {
                char savedFilename[32];
                SdStorage::savePhoto(jpgBuf, jpgLen, savedFilename, sizeof(savedFilename));
            }
        }
    }

    // Shutter press or joystick LEFT terminates Wi-Fi and returns to Menu
    JoyDirection joy = joystick.getDirection(now);
    if (btnNav.shortClicked || btnShutter.shortClicked || btnNav.longClicked || joy == JOY_LEFT) {
        ESP_LOGI(TAG, "Exiting Wi-Fi mode by user request");
        Buzzer::playUiTick();
        enterState(STATE_MENU);
    }
}

// ============================================================================
// ARDUINO SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 800));  // Short timeout for USB CDC Serial

    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "     MIphoto - COMPACT ESP32-S3 CAMERA      ");
    ESP_LOGI(TAG, "     Reset reason: %d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "=============================================");

    // Initialize piezo buzzer (Pin D7 / GPIO 44) and play startup "Mii!" immediately
    Buzzer::init();
    Buzzer::playMiiGreeting();

    // Initialize Tally LED
#if USE_TALLY_LED
    pinMode(LED_TALLY_PIN, OUTPUT);
    digitalWrite(LED_TALLY_PIN, LOW);
#endif

    // Initialize tactile input buttons and analog joystick
    btnShutter.init(BTN_SHUTTER_PIN);
    btnNav.init(BTN_NAV_PIN);
#if USE_ANALOG_JOYSTICK
    joystick.init(JOYSTICK_X_PIN, JOYSTICK_Y_PIN);
#endif

    // Initialize ST7789 IPS display on SPI3 (100% brightness by default)
    bool displayOk = DisplayUI::init();
    if (!displayOk) {
        ESP_LOGE(TAG, "Display initialization failed");
    }

    // Keep updating buzzer while splash screen is displayed
    Buzzer::delayMs(450);

    // Initialize onboard digital PDM microphone (GPIO 41 & 42)
    if (!AudioMic::init()) {
        ESP_LOGW(TAG, "Audio Mic initialization warning");
    }

    // Initialize MicroSD card slot on SPI2
    if (!SdStorage::init()) {
        ESP_LOGW(TAG, "SD Card initialization warning (no card?)");
    }

    // Initialize OV2640 camera with PSRAM framebuffers
    if (!CameraDriver::init()) {
        ESP_LOGE(TAG, "Camera initialization failed!");
        DisplayUI::showMessage("CAM ERROR", "Check Sensor B2B", 2000);
    }

    bool forceWifiTest = (DEFAULT_BOOT_WIFI_TEST_MODE == 1) || (digitalRead(BTN_SHUTTER_PIN) == LOW);

    // cppcheck-suppress knownConditionTrueFalse
    if (forceWifiTest) {
        enterState(STATE_WIFI_SERVER);
    } else {
        WiFi.persistent(false);
        WiFi.mode(WIFI_OFF);
        ESP_LOGI(TAG, "System initialization complete. Entering Live View.");
        DisplayUI::getLcd().fillScreen(0x0000);
        DisplayUI::invalidateHud();
        currentState = STATE_LIVE_VIEW;
    }
}

void loop() {
    uint32_t now = millis();

    // 100% non-blocking chiptune buzzer sound engine
    Buzzer::update(now);

    // Non-blocking button debounce updates
    btnShutter.update(now);
    btnNav.update(now);

    // Main finite state machine dispatcher
    switch (currentState) {
        case STATE_LIVE_VIEW:
            handleLiveView(now);
            break;

        case STATE_CAPTURE:
            handleCapture();
            break;

        case STATE_TIMELAPSE:
            handleTimelapse(now);
            break;

        case STATE_GALLERY:
            handleGallery();
            break;

        case STATE_MENU:
            handleMenu();
            break;

        case STATE_WIFI_SERVER:
            handleWifiServer(now);
            break;

        case STATE_MINIGAME:
            handleMiniGame(now);
            break;

        default:
            break;
    }
}
