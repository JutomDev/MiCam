#include "wifi_share.h"
#include "web_ui_assets.h"
#include "sd_storage.h"
#include "camera_driver.h"
#include "audio_mic.h"
#include "buzzer.h"
#include "esp_log.h"
#include <SD.h>

static const char* TAG = "WIFI_SHARE";

WebServer WifiShare::_server(80);
DNSServer WifiShare::_dnsServer;
bool WifiShare::_running = false;
bool WifiShare::_eventsRegistered = false;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_START:
            ESP_LOGI(TAG, "WiFi Event: SoftAP Ready | Heap: %u | PSRAM: %u", ESP.getFreeHeap(), ESP.getFreePsram());
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            ESP_LOGI(TAG, "WiFi Event: Phone connected (AID=%d) | Free Heap: %u | Min Heap: %u",
                     info.wifi_ap_staconnected.aid, ESP.getFreeHeap(), ESP.getMinFreeHeap());
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "WiFi Event: Phone disconnected (AID=%d) | Free Heap: %u", info.wifi_ap_stadisconnected.aid,
                     ESP.getFreeHeap());
            break;
        default:
            break;
    }
}

bool WifiShare::start() {
    ESP_LOGI(TAG, "Starting SoftAP: %s (Open Wi-Fi)...", WIFI_AP_SSID);

    if (!_eventsRegistered) {
        WiFi.onEvent(onWiFiEvent);
        _eventsRegistered = true;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(50);

    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : NULL;
    // Limit max_connection to 2 to bound LwIP/Wi-Fi internal SRAM allocation
    bool apOk = WiFi.softAP(WIFI_AP_SSID, pass, WIFI_AP_CHANNEL, 0, 2);
    if (!apOk) {
        ESP_LOGE(TAG, "Failed to start SoftAP!");
        WiFi.mode(WIFI_OFF);
        return false;
    }

    // Stabilize SoftAP/DHCP structures and lower RF TX power to 11 dBm to prevent
    // RF PA current spikes & brownout resets on client association (especially without antenna).
    delay(100);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.setSleep(false);

    IPAddress ip = WiFi.softAPIP();
    ESP_LOGI(TAG, "SoftAP started! IP: %s | TX Power: 11 dBm | Free Heap: %u | Free PSRAM: %u", ip.toString().c_str(),
             ESP.getFreeHeap(), ESP.getFreePsram());

    // 1. Initialize mDNS responder (accessible at http://miphoto.local)
    MDNS.end();
    if (MDNS.begin(WIFI_MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        ESP_LOGI(TAG, "mDNS responder started: http://%s.local", WIFI_MDNS_HOST);
    }

    // 2. Initialize Captive Portal DNS on port 53
    _dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer.start(53, "*", ip);
    ESP_LOGI(TAG, "DNS Captive Portal started on port 53");

    // 3. Web server route registration
    _server.on("/", HTTP_GET, handleRoot);
    _server.on("/snapshot", HTTP_GET, handleSnapshot);
    _server.on("/capture", HTTP_POST, handleCapture);
    _server.on("/preset", HTTP_GET, handlePreset);
    _server.on("/audio_level", HTTP_GET, handleAudioLevel);
    _server.on("/toggle_clap", HTTP_POST, handleToggleClap);
    _server.on("/toggle_mute", HTTP_POST, handleToggleMute);
    _server.on("/audio_threshold", HTTP_ANY, handleAudioThreshold);
    _server.on("/audio_gain", HTTP_ANY, handleAudioGain);
    _server.on("/download", HTTP_GET, handleDownload);
    _server.on("/view", HTTP_GET, handleView);
    _server.on("/delete", HTTP_POST, handleDelete);
    _server.on("/favicon.ico", HTTP_GET, handleNoContent);
    _server.on("/apple-touch-icon.png", HTTP_GET, handleNoContent);
    _server.on("/apple-touch-icon-precomposed.png", HTTP_GET, handleNoContent);
    _server.onNotFound(handleNotFound);

    _server.begin();
    _running = true;
    CameraDriver::setModeWifiStream();
    ESP_LOGI(TAG, "HTTP Server started on port 80");
    return true;
}

void WifiShare::stop() {
    if (_running) {
        _server.stop();
        _dnsServer.stop();
        MDNS.end();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        _running = false;
        ESP_LOGI(TAG, "Wi-Fi radio turned OFF (Low Power Mode restored).");
    }
}

void WifiShare::loop() {
    if (_running) {
        _dnsServer.processNextRequest();
        _server.handleClient();
    }
}

bool WifiShare::isRunning() {
    return _running;
}

String WifiShare::getIP() {
    if (_running) {
        return WiFi.softAPIP().toString();
    }
    return String("0.0.0.0");
}

void WifiShare::handleSnapshot() {
    const uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    if (!CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 80)) {
        _server.send(503, "text/plain", "Camera Busy");
        return;
    }

    _server.setContentLength(jpgLen);
    _server.send(200, "image/jpeg", "");
    _server.sendContent(reinterpret_cast<const char*>(jpgBuf), jpgLen);
}

void WifiShare::handleCapture() {
    ESP_LOGI(TAG, "Remote capture triggered via Web!");
    Buzzer::playCatMii();

    const uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    if (!CameraDriver::captureJpegStatic(&jpgBuf, &jpgLen, 92)) {
        _server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Capture failed\"}");
        return;
    }

    char savedFilename[32];
    bool ok = SdStorage::savePhoto(jpgBuf, jpgLen, savedFilename, sizeof(savedFilename));

    if (ok) {
        char json[96];
        snprintf(json, sizeof(json), "{\"status\":\"ok\",\"file\":\"%s\"}", savedFilename);
        _server.send(200, "application/json", json);
    } else {
        _server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"SD save failed\"}");
    }
}

void WifiShare::handlePreset() {
    if (_server.hasArg("id")) {
        int id = _server.arg("id").toInt();
        if (id >= 0 && id < PRESET_COUNT) {
            CameraDriver::applyPreset((CameraPreset)id);
            _server.send(200, "text/plain", CameraDriver::getPresetName((CameraPreset)id));
            return;
        }
    }
    _server.send(400, "text/plain", "Invalid preset");
}

void WifiShare::handleAudioLevel() {
    uint16_t lvl = AudioMic::getLevel();
    uint16_t peak = AudioMic::getPeakLevel();
    bool clap = AudioMic::isClapTriggerEnabled();
    uint16_t thresh = AudioMic::getThreshold();
    uint8_t gain = AudioMic::getGain();
    bool countdown = AudioMic::isCountdownActive();
    uint32_t rem = AudioMic::getCountdownRemainingMs();
    bool muted = Buzzer::isMuted();

    char json[200];
    snprintf(json, sizeof(json),
             "{\"level\":%u,\"peak\":%u,\"threshold\":%u,\"gain\":%u,"
             "\"clap_enabled\":%s,\"countdown\":%s,\"remaining_ms\":%lu,\"muted\":%s}",
             lvl, peak, thresh, gain, clap ? "true" : "false", countdown ? "true" : "false", (unsigned long)rem,
             muted ? "true" : "false");
    _server.send(200, "application/json", json);
}

void WifiShare::handleToggleClap() {
    bool current = AudioMic::isClapTriggerEnabled();
    AudioMic::setClapTriggerEnabled(!current);
    _server.send(200, "text/plain", !current ? "ENABLED" : "DISABLED");
}

void WifiShare::handleToggleMute() {
    Buzzer::toggleMute();
    _server.send(200, "text/plain", Buzzer::isMuted() ? "MUTED" : "UNMUTED");
}

void WifiShare::handleAudioThreshold() {
    if (_server.hasArg("val")) {
        int val = _server.arg("val").toInt();
        if (val >= 20 && val <= 900) {
            AudioMic::setThreshold((uint16_t)val);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", val);
            _server.send(200, "text/plain", buf);
            return;
        }
    }
    _server.send(400, "text/plain", "Invalid threshold (20-900)");
}

void WifiShare::handleAudioGain() {
    if (_server.hasArg("val")) {
        int val = _server.arg("val").toInt();
        if (val >= 1 && val <= 10) {
            AudioMic::setGain((uint8_t)val);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", val);
            _server.send(200, "text/plain", buf);
            return;
        }
    }
    _server.send(400, "text/plain", "Invalid gain (1-10)");
}

void WifiShare::handleRoot() {
    const std::vector<String>& files = SdStorage::getPhotoList();
    uint32_t totalMb = (uint32_t)(SdStorage::getTotalBytes() / (1024ULL * 1024ULL));
    uint32_t usedKb = (uint32_t)(SdStorage::getUsedBytes() / 1024ULL);

    char chunk[384];

    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, "text/html", "");

    _server.sendContent_P(HTML_HEAD);

    snprintf(chunk, sizeof(chunk),
             "<header><div class='brand'>MIPHOTO</div><div class='telemetry'>"
             "HOST: http://miphoto.local // IP: 192.168.4.1<br>"
             "MEDIA: %lu KB / %lu MB [%u FRAMES]</div></header>",
             (unsigned long)usedKb, (unsigned long)totalMb, (unsigned)files.size());
    _server.sendContent(chunk);

    _server.sendContent_P(HTML_DECK_FINDER);

    static const char* const PRESET_LABELS[PRESET_COUNT] = {"[ AUTO ISP ]", "[ SUNNY DAY ]", "[ NIGHT BOOST ]",
                                                            "[ PORTRAIT ]", "[ Y2K RETRO ]", "[ LOMO CROSS ]"};
    int curPreset = (int)CameraDriver::getCurrentPreset();
    for (int p = 0; p < PRESET_COUNT; p++) {
        snprintf(chunk, sizeof(chunk), "<button class='btn-preset%s' onclick='setPreset(%d, this)'>%s</button>",
                 (p == curPreset) ? " active" : "", p, PRESET_LABELS[p]);
        _server.sendContent(chunk);
    }

    _server.sendContent_P(HTML_DECK_AUDIO);

    bool muted = Buzzer::isMuted();
    snprintf(chunk, sizeof(chunk),
             "<button id='mute-btn' class='vu-btn' style='grid-column:span 2; color:%s;' "
             "onclick='toggleSpeakerMute()'>SPEAKER: %s</button>"
             "</div><div style='display:grid; grid-template-columns: repeat(3, 1fr); gap:6px;'>",
             muted ? "#f87171" : "#34d399", muted ? "MUTED" : "ON");
    _server.sendContent(chunk);

    _server.sendContent_P(HTML_DECK_SIM_BTNS);

    if (files.empty()) {
        _server.sendContent(
            "<div style='padding:24px; text-align:center; color:var(--muted); font-size:12px;'>"
            "ARCHIVE EMPTY. TRIGGER SHUTTER TO COMMIT FIRST FRAME.</div>");
    } else {
        _server.sendContent("<div class='film-strip'>");
        // Show newest 24 photos inline to avoid socket storms on mobile browsers
        int total = (int)files.size();
        int minIdx = (total > 24) ? (total - 24) : 0;
        for (int i = total - 1; i >= minIdx; i--) {
            const String& path = files[i];
            size_t sz = SdStorage::getPhotoSizeByIndex((size_t)i);
            float szKb = (float)sz / 1024.0f;
            int slash = path.lastIndexOf('/');
            const char* fname = (slash >= 0) ? (path.c_str() + slash + 1) : path.c_str();
            snprintf(chunk, sizeof(chunk),
                     "<div class='frame-card'>"
                     "<img src='/view?file=%s' class='frame-img' loading='lazy' decoding='async' alt='%s'>"
                     "<div class='frame-meta'><span class='frame-name'>%s</span>"
                     "<span class='frame-size'>%.1f KB</span></div>"
                     "<div class='frame-actions'>"
                     "<a href='/download?file=%s' class='btn-act btn-down'>[ GET ]</a>"
                     "<button onclick=\"deleteFile('%s')\" class='btn-act btn-del'>[ PURGE ]</button>"
                     "</div></div>",
                     path.c_str(), fname, fname, szKb, path.c_str(), path.c_str());
            _server.sendContent(chunk);
        }
        _server.sendContent("</div>");
    }

    _server.sendContent("</div></div></body></html>");
    _server.sendContent("");  // Terminate chunked transfer cleanly
}

static bool isValidMediaPath(const String& path) {
    return path.startsWith("/DCIM/") && (path.indexOf("..") < 0);
}

void WifiShare::handleDownload() {
    if (!_server.hasArg("file")) {
        _server.send(400, "text/plain", "Missing file parameter");
        return;
    }

    String path = _server.arg("file");
    if (!isValidMediaPath(path) || !SD.exists(path)) {
        _server.send(404, "text/plain", "File not found");
        return;
    }

    File file = SD.open(path, FILE_READ);
    if (!file) {
        _server.send(500, "text/plain", "Could not open file");
        return;
    }

    String filename = path.substring(path.lastIndexOf('/') + 1);
    _server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    _server.streamFile(file, "image/jpeg");
    file.close();
}

void WifiShare::handleView() {
    if (!_server.hasArg("file")) {
        _server.send(400, "text/plain", "Missing file parameter");
        return;
    }

    String path = _server.arg("file");
    if (!isValidMediaPath(path) || !SD.exists(path)) {
        _server.send(404, "text/plain", "File not found");
        return;
    }

    File file = SD.open(path, FILE_READ);
    if (!file) {
        _server.send(500, "text/plain", "Could not open file");
        return;
    }

    _server.streamFile(file, "image/jpeg");
    file.close();
}

void WifiShare::handleDelete() {
    if (!_server.hasArg("file")) {
        _server.send(400, "text/plain", "Missing file parameter");
        return;
    }

    String path = _server.arg("file");
    if (!isValidMediaPath(path)) {
        _server.send(400, "text/plain", "Invalid file path");
        return;
    }

    if (SdStorage::deletePhoto(path.c_str())) {
        _server.send(200, "text/plain", "Deleted");
    } else {
        _server.send(500, "text/plain", "Delete failed");
    }
}

void WifiShare::handleNoContent() {
    _server.send(204, "text/plain", "");
}

void WifiShare::handleNotFound() {
    // Captive Portal redirect for OS CNA probes (/generate_204, /hotspot-detect.html, etc.)
    _server.sendHeader("Location", "http://192.168.4.1/", true);
    _server.send(302, "text/plain", "Redirecting to MIphoto...");
}
