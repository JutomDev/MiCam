#include <Arduino.h>
#include "config.h"
#include "input_controls.h"
#include "camera_driver.h"
#include "display_ui.h"
#include "sd_storage.h"
#include "wifi_share.h"
#include "audio_mic.h"
#include "buzzer.h"
#include "minigame.h"
#include "sys_diag.h"
#include "esp_log.h"
#include "esp_system.h"

static const char* TAG = "MAIN_APP";

// ============================================================================
// SYSTEM STATE & HARDWARE INPUTS
// ============================================================================
static Button btnShutter;
static Button btnNav;
static AnalogJoystick joystick;

static CameraState currentState = STATE_LIVE_VIEW;
static uint8_t menuSelectedIndex = 0;
static int galleryPhotoIndex = 0;

static bool clapTriggerPending = false;
static uint32_t clapCountdownStart = 0;
static uint32_t lastFpsTime = 0;
static uint32_t frameCount = 0;

static void refreshMenu(bool fullRedraw) {
    DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                        DisplayUI::getBrightness(), Buzzer::isMuted(), SdStorage::getPhotoCount(), fullRedraw);
}

// ============================================================================
// FINITE STATE MACHINE TRANSITIONS
// ============================================================================
static void enterState(CameraState newState) {
    if (currentState == newState) return;

    ESP_LOGI(TAG, "FSM Transition: %d -> %d", (int)currentState, (int)newState);

    joystick.waitRelease();
    btnShutter.clearEvents();
    btnNav.clearEvents();

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
            frameCount = 0;
            lastFpsTime = millis();
            break;

        case STATE_GALLERY: {
            Buzzer::stop();
            if (SdStorage::isAvailable() && SdStorage::getPhotoCount() == 0) {
                SdStorage::refreshList();
            }
            uint16_t count = SdStorage::getPhotoCount();
            if (count == 0) {
                DisplayUI::drawGalleryEmpty();
                galleryPhotoIndex = 0;
            } else {
                galleryPhotoIndex = static_cast<int>(count) - 1;
                DisplayUI::drawGalleryPhoto(SdStorage::getPhotoPath(galleryPhotoIndex), galleryPhotoIndex + 1, count);
            }
            break;
        }

        case STATE_MENU:
            refreshMenu(true);
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
            MiniGame::init();
            break;

        default:
            break;
    }
}

// ============================================================================
// INDIVIDUAL STATE HANDLERS
// ============================================================================
static void handleCapture() {
    ESP_LOGI(TAG, "Executing photo capture...");
    SysDiag::logReport(TAG, "PRE_CAPTURE", -1.0f, true);
    Buzzer::playCatMii();

#if USE_TALLY_LED
    digitalWrite(LED_TALLY_PIN, HIGH);
#endif

    DisplayUI::drawFlashAnimation();
    DisplayUI::drawSavingProgress((CameraDriver::getCurrentPreset() == PRESET_NIGHT) ? "NIGHT BURST 5x..."
                                                                                     : "ENCODING JPEG...");

    const uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    if (!CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 92)) {
        ESP_LOGE(TAG, "Capture failed!");
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        DisplayUI::showMessage("CAM ERROR", "Capture failed", 700);
        SysDiag::logReport(TAG, "POST_CAPTURE_ERR", -1.0f, true);
        enterState(STATE_LIVE_VIEW);
        return;
    }

    Buzzer::update();

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

    SysDiag::logReport(TAG, "POST_CAPTURE", -1.0f, true);
    enterState(STATE_LIVE_VIEW);
}

static void handleLiveView(uint32_t now) {
    JoyDirection joy = joystick.getDirection(now);

    if (btnShutter.shortClicked) {
        enterState(STATE_CAPTURE);
        return;
    }

    if (btnNav.shortClicked || joy == JOY_RIGHT || joy == JOY_LEFT) {
        Buzzer::playUiTick();
        CameraDriver::nextPreset();
        DisplayUI::invalidateHud();
    }

    if (btnNav.longClicked || btnShutter.longClicked || joy == JOY_UP || joy == JOY_DOWN) {
        Buzzer::playUiTick();
        enterState(STATE_MENU);
        return;
    }

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

    // Pace Live View polling to <= ~18 FPS (55 ms) to reduce SPI3/PSRAM DMA contention & heat
    static uint32_t lastFrameTime = 0;
    if (now - lastFrameTime < 55) {
        return;
    }
    lastFrameTime = now;

    camera_fb_t* fb = CameraDriver::getFrame();
    if (fb) {
        DisplayUI::drawLiveView(fb->buf, fb->width, fb->height,
                                CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                SdStorage::getPhotoCount(), SdStorage::isAvailable());
        CameraDriver::returnFrame(fb);

        frameCount++;
        if (now - lastFpsTime >= 5000) {
            float fps = frameCount * 1000.0f / (now - lastFpsTime);
            SysDiag::logReport(TAG, "LIVE_VIEW", fps, false);
            frameCount = 0;
            lastFpsTime = now;
        }
    } else {
        DisplayUI::drawHud(CameraDriver::getPresetName(CameraDriver::getCurrentPreset()), SdStorage::getPhotoCount(),
                           SdStorage::isAvailable());
    }
}

static void handleGallery() {
    uint16_t count = SdStorage::getPhotoCount();
    JoyDirection joy = joystick.getDirection(millis());

    if (count == 0) {
        if (btnNav.shortClicked || btnShutter.shortClicked || joy != JOY_NONE) {
            Buzzer::playUiTick();
            enterState(STATE_MENU);
        }
        return;
    }

    if (galleryPhotoIndex >= static_cast<int>(count)) {
        galleryPhotoIndex = static_cast<int>(count) - 1;
    }
    if (galleryPhotoIndex < 0) {
        galleryPhotoIndex = 0;
    }

    if (btnShutter.longClicked) {
        char toDelete[32];
        strncpy(toDelete, SdStorage::getPhotoPath(galleryPhotoIndex), sizeof(toDelete) - 1);
        toDelete[sizeof(toDelete) - 1] = '\0';
        ESP_LOGI(TAG, "Deleting photo from gallery: %s", toDelete);
        Buzzer::playCountdownTick();
        DisplayUI::showMessage("DELETING...", toDelete, 100);
        SdStorage::deletePhoto(toDelete);
        count = SdStorage::getPhotoCount();
        if (count == 0) {
            DisplayUI::drawGalleryEmpty();
            galleryPhotoIndex = 0;
        } else {
            if (galleryPhotoIndex >= static_cast<int>(count)) {
                galleryPhotoIndex = static_cast<int>(count) - 1;
            }
            DisplayUI::drawGalleryPhoto(SdStorage::getPhotoPath(galleryPhotoIndex), galleryPhotoIndex + 1, count);
        }
        return;
    }

    if (btnNav.shortClicked || joy == JOY_LEFT) {
        Buzzer::playUiTick();
        galleryPhotoIndex = (galleryPhotoIndex > 0) ? (galleryPhotoIndex - 1) : (static_cast<int>(count) - 1);
        DisplayUI::drawGalleryPhoto(SdStorage::getPhotoPath(galleryPhotoIndex), galleryPhotoIndex + 1, count);
    } else if (btnShutter.shortClicked || joy == JOY_RIGHT) {
        Buzzer::playUiTick();
        galleryPhotoIndex = (galleryPhotoIndex + 1) % static_cast<int>(count);
        DisplayUI::drawGalleryPhoto(SdStorage::getPhotoPath(galleryPhotoIndex), galleryPhotoIndex + 1, count);
    } else if (btnNav.longClicked || joy == JOY_UP || joy == JOY_DOWN) {
        Buzzer::playUiTick();
        enterState(STATE_MENU);
    }
}

static void handleMenu() {
    constexpr uint8_t MENU_ITEMS_COUNT = 7;
    JoyDirection joy = joystick.getDirection(millis());

    if (btnNav.shortClicked || joy == JOY_DOWN) {
        Buzzer::playUiTick();
        menuSelectedIndex = (menuSelectedIndex + 1) % MENU_ITEMS_COUNT;
        refreshMenu(false);
        return;
    }
    if (joy == JOY_UP) {
        Buzzer::playUiTick();
        menuSelectedIndex = (menuSelectedIndex == 0) ? (MENU_ITEMS_COUNT - 1) : (menuSelectedIndex - 1);
        refreshMenu(false);
        return;
    }

    if (btnShutter.shortClicked || joy == JOY_RIGHT) {
        switch (menuSelectedIndex) {
            case 0:  // Cycle image preset
                Buzzer::playUiTick();
                CameraDriver::nextPreset();
                refreshMenu(false);
                break;

            case 1:  // Cycle screen brightness
                Buzzer::playUiTick();
                DisplayUI::cycleBrightness();
                refreshMenu(false);
                break;

            case 2:  // Toggle speaker mute (default: MUTED)
                Buzzer::toggleMute();
                Buzzer::playUiTick();
                refreshMenu(false);
                break;

            case 3:  // Open SD photo gallery
                Buzzer::playUiTick();
                enterState(STATE_GALLERY);
                break;

            case 4:  // Launch SoftAP Wi-Fi file server
                Buzzer::playUiTick();
                enterState(STATE_WIFI_SERVER);
                break;

            case 5:  // Launch Retro Mini-Game: Mii-Snake
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

    if (btnNav.longClicked || btnShutter.longClicked || joy == JOY_LEFT) {
        Buzzer::playUiTick();
        enterState(STATE_LIVE_VIEW);
    }
}

static void handleWifiServer(uint32_t now) {
    WifiShare::loop();

    AudioMic::update(now);
    if (AudioMic::checkClapDetected() && !clapTriggerPending) {
        ESP_LOGI(TAG, "Clap detected in Wi-Fi mode! Exposing in 2s...");
        clapTriggerPending = true;
        clapCountdownStart = now;
        AudioMic::setCountdownStatus(true, 2000);
        Buzzer::playCountdownTick();
    }

    if (clapTriggerPending) {
        uint32_t elapsed = now - clapCountdownStart;
        uint32_t rem = (elapsed < 2000) ? (2000 - elapsed) : 0;
        AudioMic::setCountdownStatus(true, rem);
        if (elapsed >= 2000) {
            clapTriggerPending = false;
            AudioMic::setCountdownStatus(false, 0);
            Buzzer::playCatMii();

            const uint8_t* jpgBuf = nullptr;
            size_t jpgLen = 0;
            if (CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 92)) {
                char savedFilename[32];
                SdStorage::savePhoto(jpgBuf, jpgLen, savedFilename, sizeof(savedFilename));
            }
        }
    }

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
    while (!Serial && (millis() - t0 < 800));

    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "     MIphoto - COMPACT ESP32-S3 CAMERA      ");
    ESP_LOGI(TAG, "     Reset reason: %d", static_cast<int>(esp_reset_reason()));
    ESP_LOGI(TAG, "=============================================");

    Buzzer::init();
    Buzzer::playMiiGreeting();

#if USE_TALLY_LED
    pinMode(LED_TALLY_PIN, OUTPUT);
    digitalWrite(LED_TALLY_PIN, LOW);
#endif

    btnShutter.init(BTN_SHUTTER_PIN);
    btnNav.init(BTN_NAV_PIN);
#if USE_ANALOG_JOYSTICK
    joystick.init(JOYSTICK_X_PIN, JOYSTICK_Y_PIN);
#endif

    if (!DisplayUI::init()) {
        ESP_LOGE(TAG, "Display initialization failed");
    }

    Buzzer::delayMs(450);

    if (!AudioMic::init()) {
        ESP_LOGW(TAG, "Audio Mic initialization warning");
    }

    if (!SdStorage::init()) {
        ESP_LOGW(TAG, "SD Card initialization warning (no card?)");
    }

    if (!CameraDriver::init()) {
        ESP_LOGE(TAG, "Camera initialization failed!");
        DisplayUI::showMessage("CAM ERROR", "Check Sensor B2B", 2000);
    }

    bool forceWifiTest = (DEFAULT_BOOT_WIFI_TEST_MODE == 1) || (digitalRead(BTN_SHUTTER_PIN) == LOW);
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

    Buzzer::update(now);
    btnShutter.update(now);
    btnNav.update(now);

    switch (currentState) {
        case STATE_LIVE_VIEW:
            handleLiveView(now);
            break;

        case STATE_CAPTURE:
            handleCapture();
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
            if (MiniGame::update(now, joystick, btnShutter)) {
                enterState(STATE_MENU);
            }
            break;

        default:
            break;
    }
}
