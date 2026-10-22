#include <Arduino.h>
#include "config.h"
#include "camera_driver.h"
#include "display_ui.h"
#include "sd_storage.h"
#include "wifi_share.h"
#include "audio_mic.h"
#include "buzzer.h"
#include "esp_log.h"

static const char* TAG = "MAIN_APP";

// Acoustic clap trigger state variables
static bool clapTriggerPending = false;
static uint32_t clapCountdownStart = 0;

// ============================================================================
// NON-BLOCKING BUTTON ENGINE (Debounce & Long Press based on millis())
// ============================================================================
struct Button {
    uint8_t pin;
    bool lastState;
    bool isPressed;
    uint32_t pressStartTime;
    bool longPressFired;
    bool shortClicked;
    bool longClicked;

    void init(uint8_t p) {
        pin = p;
        pinMode(pin, INPUT_PULLUP);
        lastState = HIGH;
        isPressed = false;
        pressStartTime = 0;
        longPressFired = false;
        shortClicked = false;
        longClicked = false;
    }

    void update(uint32_t now) {
        shortClicked = false;
        longClicked = false;

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

static Button btnShutter;
static Button btnNav;

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
void enterState(CameraState newState);

// ============================================================================
// FSM STATE TRANSITION HELPERS
// ============================================================================
void enterState(CameraState newState) {
    if (currentState == newState) return;

    ESP_LOGI(TAG, "FSM Transition: %d -> %d", (int)currentState, (int)newState);

    // Teardown previous state
    if (currentState == STATE_WIFI_SERVER) {
        WifiShare::stop();
        // Restore camera preview mode
        CameraDriver::setModePreview();
    }

    currentState = newState;

    switch (currentState) {
        case STATE_LIVE_VIEW:
            CameraDriver::setModePreview();
            break;

        case STATE_GALLERY: {
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
            menuSelectedIndex = 0;
            DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                DisplayUI::getBrightness());
            break;

        case STATE_WIFI_SERVER:
            // Switch camera sensor to fast QVGA JPEG stream for mobile browser preview
            CameraDriver::setModeWifiStream();
            DisplayUI::showMessage("STARTING WI-FI", "Connecting...", 200);
            if (WifiShare::start()) {
                DisplayUI::drawWifiServerScreen(WIFI_AP_SSID, WifiShare::getIP().c_str(), SdStorage::getPhotoCount());
            } else {
                DisplayUI::showMessage("WI-FI ERROR", "Failed to start AP", 1200);
                enterState(STATE_MENU);
            }
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
    // 1. Handle user inputs
    if (btnShutter.shortClicked) {
        enterState(STATE_CAPTURE);
        return;
    }

    if (btnNav.shortClicked) {
        // Fast cycling through camera presets on the fly
        CameraDriver::nextPreset();
    }

    if (btnNav.longClicked) {
        // Long-press opens main settings menu
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

        // Rhythmic Tally LED blink countdown:
        // First 1.2s: steady pulse at 250ms interval
        // Final 0.8s: rapid strobe at 100ms interval
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
    Buzzer::playShutterClick();

#if USE_TALLY_LED
    digitalWrite(LED_TALLY_PIN, HIGH);
#endif

    // 1. White screen flash animation
    DisplayUI::drawFlashAnimation();

    // 2. Switch sensor to high-resolution UXGA JPEG mode
    DisplayUI::drawSavingProgress("CAPTURING UXGA");
    if (!CameraDriver::setModeCapture()) {
        ESP_LOGE(TAG, "Failed to switch to UXGA capture mode");
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        enterState(STATE_LIVE_VIEW);
        return;
    }

    // 3. Capture image frame
    camera_fb_t* fb = CameraDriver::getFrame();
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        ESP_LOGE(TAG, "Capture failed! Bad frame buffer");
        if (fb) CameraDriver::returnFrame(fb);
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        enterState(STATE_LIVE_VIEW);
        return;
    }

    // 4. Save to SD card file with embedded EXIF metadata
    char savedFilename[32];
    DisplayUI::drawSavingProgress("WRITING TO SD");
    bool saved = SdStorage::savePhoto(fb->buf, fb->len, savedFilename, sizeof(savedFilename));

    // 5. Always return frame buffer to PSRAM pool (Rule: zero memory leaks)
    CameraDriver::returnFrame(fb);

#if USE_TALLY_LED
    digitalWrite(LED_TALLY_PIN, LOW);
#endif

    if (saved) {
        ESP_LOGI(TAG, "Photo successfully saved: %s", savedFilename);
        Buzzer::playSavedSuccess();
#if USE_TALLY_LED
        // Double blink on Tally LED confirming valid photo write
        digitalWrite(LED_TALLY_PIN, HIGH);
        delay(50);
        digitalWrite(LED_TALLY_PIN, LOW);
        delay(50);
        digitalWrite(LED_TALLY_PIN, HIGH);
        delay(50);
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        DisplayUI::showMessage("SAVED!", savedFilename, 350);
    } else {
        ESP_LOGE(TAG, "Failed to save photo to SD!");
        DisplayUI::showMessage("SD ERROR!", "Photo not saved", 1000);
    }

    // 6. Return to Live View
    enterState(STATE_LIVE_VIEW);
}

static uint32_t lastTimelapseShot = 0;
static uint16_t timelapseCount = 0;
const uint32_t TIMELAPSE_INTERVAL_MS = 5000;  // 5-second intervalometer

void handleTimelapse(uint32_t now) {
    // Any button press terminates timelapse and returns to Live View
    if (btnNav.shortClicked || btnShutter.shortClicked || btnNav.longClicked) {
        ESP_LOGI(TAG, "Exiting Timelapse mode");
        enterState(STATE_LIVE_VIEW);
        return;
    }

    // Elapsed check for next intervalometer frame
    uint32_t elapsed = now - lastTimelapseShot;
    if (elapsed >= TIMELAPSE_INTERVAL_MS) {
        lastTimelapseShot = now;
        timelapseCount++;
        ESP_LOGI(TAG, "Time-lapse capture #%d", timelapseCount);

        // Warning pre-flash on Tally LED
#if USE_TALLY_LED
        digitalWrite(LED_TALLY_PIN, HIGH);
        delay(80);
        digitalWrite(LED_TALLY_PIN, LOW);
#endif
        handleCapture();
    }
}

void handleGallery() {
    std::vector<String> list = SdStorage::getPhotoList();
    if (list.empty()) {
        if (btnNav.shortClicked || btnShutter.shortClicked) {
            enterState(STATE_LIVE_VIEW);
        }
        return;
    }

    // Short-press NAV - previous image
    if (btnNav.shortClicked) {
        if (galleryPhotoIndex > 0) {
            galleryPhotoIndex--;
        } else {
            galleryPhotoIndex = list.size() - 1;  // Wrap around
        }
        DisplayUI::drawGalleryPhoto(list[galleryPhotoIndex].c_str(), galleryPhotoIndex + 1, list.size());
    }

    // Short-press SHUTTER - return to Live View
    if (btnShutter.shortClicked || btnNav.longClicked) {
        enterState(STATE_LIVE_VIEW);
    }
}

void handleMenu() {
    // Downwards navigation through menu options
    if (btnNav.shortClicked) {
        menuSelectedIndex = (menuSelectedIndex + 1) % 5;
        DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                            DisplayUI::getBrightness());
    }

    // Select highlighted item (SHUTTER)
    if (btnShutter.shortClicked) {
        switch (menuSelectedIndex) {
            case 0:  // Cycle image preset
                CameraDriver::nextPreset();
                DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                    DisplayUI::getBrightness());
                break;

            case 1:  // Cycle screen PWM brightness
                DisplayUI::cycleBrightness();
                DisplayUI::drawMenu(menuSelectedIndex, CameraDriver::getPresetName(CameraDriver::getCurrentPreset()),
                                    DisplayUI::getBrightness());
                break;

            case 2:  // Launch SoftAP Wi-Fi file server
                enterState(STATE_WIFI_SERVER);
                break;

            case 3:  // Open SD photo gallery
                enterState(STATE_GALLERY);
                break;

            case 4:  // Return to Live View
                enterState(STATE_LIVE_VIEW);
                break;
        }
    }

    // Long-press NAV immediately exits to camera
    if (btnNav.longClicked) {
        enterState(STATE_LIVE_VIEW);
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
            Buzzer::playShutterClick();
            ESP_LOGI(TAG, "Executing clap capture in current preset: %s",
                     CameraDriver::getPresetName(CameraDriver::getCurrentPreset()));

            // Capture full UXGA JPEG frame with active preset
            CameraDriver::setModeCapture();
            camera_fb_t* fb = CameraDriver::getFrame();
            if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                char savedFilename[32];
                bool ok = SdStorage::savePhoto(fb->buf, fb->len, savedFilename, sizeof(savedFilename));
                CameraDriver::returnFrame(fb);
                if (ok) {
                    ESP_LOGI(TAG, "Photo saved via clap: %s", savedFilename);
                    Buzzer::playSavedSuccess();
#if USE_TALLY_LED
                    // Confirmation double-blink
                    digitalWrite(LED_TALLY_PIN, HIGH);
                    delay(50);
                    digitalWrite(LED_TALLY_PIN, LOW);
                    delay(50);
                    digitalWrite(LED_TALLY_PIN, HIGH);
                    delay(50);
                    digitalWrite(LED_TALLY_PIN, LOW);
#endif
                }
            } else {
                if (fb) CameraDriver::returnFrame(fb);
            }
            CameraDriver::setModeWifiStream();  // Return to Wi-Fi live stream
        }
    }

    // Any button press terminates Wi-Fi and returns to Live View
    if (btnNav.shortClicked || btnShutter.shortClicked || btnNav.longClicked) {
        ESP_LOGI(TAG, "Exiting Wi-Fi mode by user request");
        enterState(STATE_LIVE_VIEW);
    }
}

// ============================================================================
// ARDUINO SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 1500));  // Short timeout for USB CDC Serial

    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "     MIphoto - COMPACT ESP32-S3 CAMERA      ");
    ESP_LOGI(TAG, "=============================================");

    // Initialize piezo buzzer (Pin D5 / GPIO 6)
    Buzzer::init();
    Buzzer::playMiiGreeting();  // Iconic "Mii-ii!" startup chime

    // Initialize Tally LED
#if USE_TALLY_LED
    pinMode(LED_TALLY_PIN, OUTPUT);
    digitalWrite(LED_TALLY_PIN, LOW);
#endif

    // Initialize tactile input buttons
    btnShutter.init(BTN_SHUTTER_PIN);
    btnNav.init(BTN_NAV_PIN);

    // Initialize onboard digital PDM microphone (GPIO 41 & 42)
    if (!AudioMic::init()) {
        ESP_LOGW(TAG, "Audio Mic initialization warning");
    }

    // Initialize ST7789 IPS display on SPI3
    bool displayOk = DisplayUI::init();
    if (!displayOk) {
        ESP_LOGE(TAG, "Display initialization failed (no screen attached?)");
    }

    // Initialize MicroSD card slot on SPI2
    if (!SdStorage::init()) {
        ESP_LOGW(TAG, "SD Card initialization warning (no card?)");
    }

    // Initialize OV2640 camera with PSRAM framebuffers
    if (!CameraDriver::init()) {
        ESP_LOGE(TAG, "Camera initialization failed!");
        DisplayUI::showMessage("CAM ERROR", "Check Sensor B2B", 3000);
    }

    // OUT-OF-THE-BOX BOOT MODE:
    // If DEFAULT_BOOT_WIFI_TEST_MODE == 1 OR shutter button held down at boot:
    bool forceWifiTest = (DEFAULT_BOOT_WIFI_TEST_MODE == 1) || (digitalRead(BTN_SHUTTER_PIN) == LOW);

    // cppcheck-suppress knownConditionTrueFalse
    if (forceWifiTest) {
        ESP_LOGI(TAG, "==========================================================");
        ESP_LOGI(TAG, " [OUT-OF-THE-BOX / HEADLESS WI-FI TEST MODE LAUNCHED]     ");
        ESP_LOGI(TAG, " 1. Connect phone to Wi-Fi SSID: %s (OPEN / NO PASSWORD) ", WIFI_AP_SSID);
        ESP_LOGI(TAG, " 2. Navigate in browser to: http://%s.local                ", WIFI_MDNS_HOST);
        ESP_LOGI(TAG, "    (or direct IP: http://192.168.4.1)                     ");
        ESP_LOGI(TAG, " 3. View live stream, trigger shutter, check mic VU meter  ");
        ESP_LOGI(TAG, "    and download photos directly from MicroSD card!        ");
        ESP_LOGI(TAG, "==========================================================");
        enterState(STATE_WIFI_SERVER);
    } else {
        WiFi.mode(WIFI_OFF);
        ESP_LOGI(TAG, "System initialization complete. Entering Live View.");
        enterState(STATE_LIVE_VIEW);
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
    }
}
