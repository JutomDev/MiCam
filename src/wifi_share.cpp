#include "wifi_share.h"
#include "web_ui_assets.h"
#include "sd_storage.h"
#include "camera_driver.h"
#include "audio_mic.h"
#include "buzzer.h"
#include "sys_diag.h"
#include <esp_heap_caps.h>
#include "esp_log.h"
#include <SD.h>

static const char* TAG = "WIFI_SHARE";

WebServer WifiShare::_server(80);
DNSServer WifiShare::_dnsServer;
bool WifiShare::_running = false;
bool WifiShare::_eventsRegistered = false;

static constexpr size_t STREAM_CHUNK_BUF_SIZE = 4096;
static uint8_t* s_streamChunkBuf = nullptr;

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

    // Switch camera to calm Hardware JPEG pipeline BEFORE bringing up Wi-Fi RF
    // so 22 FPS RGB565 DMA interrupts do not contend with Core 0 Wi-Fi initialization.
    if (!CameraDriver::setModeWifiStream()) {
        ESP_LOGW(TAG, "Camera Wi-Fi stream mode init warning");
    }

    if (!_eventsRegistered) {
        WiFi.onEvent(onWiFiEvent);
        _eventsRegistered = true;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
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

    // Stabilize SoftAP/DHCP structures and clamp RF TX power to 8.5 dBm to prevent
    // LDO brownout / current spikes on client association.
    delay(100);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.setSleep(false);

    IPAddress ip = WiFi.softAPIP();
    ESP_LOGI(TAG, "SoftAP started! IP: %s | TX Power: 8.5 dBm | Free Heap: %u | Free PSRAM: %u", ip.toString().c_str(),
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

    // Allocate Wi-Fi file streaming chunk buffer in PSRAM instead of reserving 4 KB internal SRAM
    if (!s_streamChunkBuf) {
        s_streamChunkBuf =
            static_cast<uint8_t*>(heap_caps_malloc(STREAM_CHUNK_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    // 3. Web server route registration
    _server.on("/", HTTP_GET, handleRoot);
    _server.on("/gallery", HTTP_GET, handleGalleryPage);
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
    _server.on("/diag", HTTP_GET, handleDiag);

    // Trigger smartphone/PC Captive Portal auto-launch immediately on Wi-Fi connect
    _server.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
    _server.on("/gen_204", HTTP_GET, handleCaptiveRedirect);
    _server.on("/ncsi.txt", HTTP_GET, handleCaptiveRedirect);
    _server.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);
    _server.on("/redirect", HTTP_GET, handleCaptiveRedirect);
    _server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
    _server.on("/library/test/success.html", HTTP_GET, handleCaptiveRedirect);
    _server.on("/success.txt", HTTP_GET, handleCaptiveRedirect);
    _server.on("/canonical.html", HTTP_GET, handleCaptiveRedirect);
    _server.on("/mobile/status.php", HTTP_GET, handleCaptiveRedirect);

    // Silence missing browser icon/metadata requests without redirecting to /
    _server.on("/favicon.ico", HTTP_GET, handleNoContent);
    _server.on("/apple-touch-icon.png", HTTP_GET, handleNoContent);
    _server.on("/apple-touch-icon-precomposed.png", HTTP_GET, handleNoContent);
    _server.on("/robots.txt", HTTP_GET, handleNoContent);
    _server.onNotFound(handleNotFound);

    _server.begin();
    _running = true;
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
    if (s_streamChunkBuf) {
        heap_caps_free(s_streamChunkBuf);
        s_streamChunkBuf = nullptr;
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
    if (!CameraDriver::captureStreamJpegStatic(&jpgBuf, &jpgLen)) {
        _server.send(503, "text/plain", "Camera Busy");
        return;
    }

    WiFiClient client = _server.client();
    client.setTimeout(1);
    _server.setContentLength(jpgLen);
    _server.send(200, "image/jpeg", "");
    _server.sendContent(reinterpret_cast<const char*>(jpgBuf), jpgLen);
}

void WifiShare::handleCapture() {
    ESP_LOGI(TAG, "Remote capture triggered via Web (2MP UXGA)!");
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
    uint16_t photoCount = SdStorage::getPhotoCount();
    uint32_t totalMb = (uint32_t)(SdStorage::getTotalBytes() / (1024ULL * 1024ULL));
    uint32_t usedKb = (uint32_t)(SdStorage::getUsedBytes() / 1024ULL);

    char chunk[1024];

    _server.client().setTimeout(1);
    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, "text/html", "");

    _server.sendContent(
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>MIphoto Camera</title>");
    _server.sendContent_P(HTML_SHARED_STYLE);
    _server.sendContent_P(HTML_HEAD_SCRIPTS);

    static const char* const PRESET_LABELS[PRESET_COUNT] = {"Auto",     "Sunny",     "Night 5x",
                                                            "Portrait", "Retro Y2K", "Lomo"};
    int curPreset = (int)CameraDriver::getCurrentPreset();
    const char* activePresetName = (curPreset >= 0 && curPreset < PRESET_COUNT) ? PRESET_LABELS[curPreset] : "Auto";

    snprintf(chunk, sizeof(chunk),
             "<script>frameTotal=%u;document.addEventListener('DOMContentLoaded',()=>{"
             "let pb=document.getElementById('preset-badge');if(pb)pb.innerText='%s';});</script>",
             (unsigned)photoCount, activePresetName);
    _server.sendContent(chunk);

    snprintf(chunk, sizeof(chunk),
             "<header><div class='brand-wrap'><span class='brand-dot'></span><span class='brand'>MIphoto</span></div>"
             "<div class='telemetry'><strong>miphoto.local</strong> · 192.168.4.1<br>"
             "%lu KB / %lu MB · <span id='hdr-cnt'>%u</span> photos</div></header>",
             (unsigned long)usedKb, (unsigned long)totalMb, (unsigned)photoCount);
    _server.sendContent(chunk);

    snprintf(chunk, sizeof(chunk),
             "<nav class='tabs'>"
             "<a href='/' class='tab active'>Live Camera</a>"
             "<a href='/gallery' class='tab'>Gallery <span id='gal-cnt' class='tab-badge'>%u</span></a>"
             "</nav>",
             (unsigned)photoCount);
    _server.sendContent(chunk);

    _server.sendContent_P(HTML_DECK_FINDER);

    for (int p = 0; p < PRESET_COUNT; p++) {
        snprintf(chunk, sizeof(chunk), "<button class='btn-preset%s' onclick='setPreset(%d, this)'>%s</button>",
                 (p == curPreset) ? " active" : "", p, PRESET_LABELS[p]);
        _server.sendContent(chunk);
    }

    _server.sendContent_P(HTML_DECK_AUDIO);

    bool muted = Buzzer::isMuted();
    snprintf(chunk, sizeof(chunk),
             "<button id='mute-btn' class='chip-btn %s' onclick='toggleSpeakerMute()'>Sound: %s</button>",
             muted ? "chip-off" : "chip-on", muted ? "Muted" : "On");
    _server.sendContent(chunk);

    _server.sendContent_P(HTML_DECK_SIM_BTNS);

    _server.sendContent("</div></body></html>");
    _server.sendContent("");  // Terminate chunked transfer cleanly
}

void WifiShare::handleGalleryPage() {
    uint16_t photoCount = SdStorage::getPhotoCount();
    uint32_t totalMb = (uint32_t)(SdStorage::getTotalBytes() / (1024ULL * 1024ULL));
    uint32_t usedKb = (uint32_t)(SdStorage::getUsedBytes() / 1024ULL);

    constexpr int PAGE_SIZE = 6;
    int total = (int)photoCount;
    int totalPages = (total > 0) ? ((total + PAGE_SIZE - 1) / PAGE_SIZE) : 1;
    int page = 1;
    if (_server.hasArg("page")) {
        page = _server.arg("page").toInt();
        if (page < 1) page = 1;
        if (page > totalPages) page = totalPages;
    }

    char chunk[1024];

    _server.client().setTimeout(1);
    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, "text/html", "");

    _server.sendContent(
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>MIphoto Gallery</title>");
    _server.sendContent_P(HTML_SHARED_STYLE);
    _server.sendContent_P(HTML_GALLERY_SCRIPTS);

    snprintf(chunk, sizeof(chunk),
             "<header><div class='brand-wrap'><span class='brand-dot'></span><span class='brand'>MIphoto</span></div>"
             "<div class='telemetry'><strong>SD Archive</strong><br>"
             "%lu KB / %lu MB · %d photos</div></header>",
             (unsigned long)usedKb, (unsigned long)totalMb, total);
    _server.sendContent(chunk);

    snprintf(chunk, sizeof(chunk),
             "<nav class='tabs'>"
             "<a href='/' class='tab'>Live Camera</a>"
             "<a href='/gallery' class='tab active'>Gallery <span class='tab-badge'>%d</span></a>"
             "</nav>"
             "<div class='card'>"
             "<div class='card-head'><span class='card-title'>Saved Photos</span>"
             "<span class='card-sub'>Page %d of %d</span></div>",
             total, page, totalPages);
    _server.sendContent(chunk);

    if (photoCount == 0) {
        _server.sendContent(
            "<div style='padding:28px 12px; text-align:center; color:var(--muted); font-size:13px;'>"
            "No photos saved yet. Switch to Live Camera and take a photo.</div>");
    } else {
        int startFromTop = (page - 1) * PAGE_SIZE;
        int endFromTop = startFromTop + PAGE_SIZE;
        if (endFromTop > total) endFromTop = total;

        _server.sendContent("<div class='gallery-grid'>");
        for (int offset = startFromTop; offset < endFromTop; offset++) {
            int i = total - 1 - offset;  // Newest first
            const char* path = SdStorage::getPhotoPath((size_t)i);
            size_t sz = SdStorage::getPhotoSizeByIndex((size_t)i);
            float szKb = (float)sz / 1024.0f;
            const char* slash = strrchr(path, '/');
            const char* fname = slash ? (slash + 1) : path;
            snprintf(chunk, sizeof(chunk),
                     "<div class='frame-card'>"
                     "<div class='frame-thumb' onclick=\"openPreview('/view?file=%s&sz=%u','/download?file=%s','%s')\">"
                     "<span class='frame-placeholder'>Loading...</span>"
                     "<img data-src='/view?file=%s&sz=%u' class='frame-img' alt='%s'></div>"
                     "<div class='frame-meta'><span class='frame-name mono'>%s</span>"
                     "<span class='frame-size mono'>%.0f KB</span></div>"
                     "<div class='frame-actions'>"
                     "<a href='/download?file=%s' class='btn btn-down'>Save</a>"
                     "<button onclick=\"handleDeleteClick(this,'%s')\" class='btn btn-del'>Del</button>"
                     "</div></div>",
                     path, (unsigned)sz, path, fname, path, (unsigned)sz, fname, fname, szKb, path, path);
            _server.sendContent(chunk);
        }
        _server.sendContent("</div>");

        snprintf(chunk, sizeof(chunk),
                 "<div class='pager'>"
                 "<a href='/gallery?page=%d' class='pager-btn%s'>Prev</a>"
                 "<span class='card-sub'>Page %d of %d</span>"
                 "<a href='/gallery?page=%d' class='pager-btn%s'>Next</a>"
                 "</div>",
                 (page > 1) ? (page - 1) : 1, (page <= 1) ? " disabled" : "", page, totalPages,
                 (page < totalPages) ? (page + 1) : totalPages, (page >= totalPages) ? " disabled" : "");
        _server.sendContent(chunk);
    }

    _server.sendContent("</div></div></body></html>");
    _server.sendContent("");
}

static bool isValidMediaPath(const String& path) {
    return path.startsWith("/DCIM/") && (path.indexOf("..") < 0);
}

static void streamFileSafe(WebServer& server, File& file, const char* contentType) {
    WiFiClient client = server.client();
    client.setTimeout(1);

    size_t fileSize = file.size();
    server.setContentLength(fileSize);
    server.send(200, contentType, "");

    uint8_t fallbackBuf[512];
    uint8_t* chunkBuf = s_streamChunkBuf ? s_streamChunkBuf : fallbackBuf;
    size_t chunkCap = s_streamChunkBuf ? STREAM_CHUNK_BUF_SIZE : sizeof(fallbackBuf);

    while (file.available()) {
        if (!client.connected()) {
            break;
        }
        size_t n = file.read(chunkBuf, chunkCap);
        if (n == 0) {
            break;
        }
        size_t sent = client.write(chunkBuf, n);
        if (sent == 0 || !client.connected()) {
            break;
        }
        yield();
    }
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
    streamFileSafe(_server, file, "image/jpeg");
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

    _server.sendHeader("Cache-Control", "public, max-age=3600");
    streamFileSafe(_server, file, "image/jpeg");
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

void WifiShare::handleDiag() {
    SysDiagSnapshot s = SysDiag::capture(true);
    char json[384];
    snprintf(json, sizeof(json),
             "{\"uptime_ms\":%lu,\"sram_free\":%u,\"sram_largest_block\":%u,\"sram_min_free\":%u,"
             "\"sram_frag_pct\":%u,\"psram_free\":%u,\"psram_largest_block\":%u,\"psram_min_free\":%u,"
             "\"psram_frag_pct\":%u,\"stack_high_water_bytes\":%u,\"heap_integrity_ok\":%s}",
             (unsigned long)millis(), (unsigned)s.sramFree, (unsigned)s.sramLargest, (unsigned)s.sramMin,
             (unsigned)s.sramFragPct, (unsigned)s.psramFree, (unsigned)s.psramLargest, (unsigned)s.psramMin,
             (unsigned)s.psramFragPct, (unsigned)s.stackHighWaterBytes, s.heapIntegrityOk ? "true" : "false");
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", json);
}

void WifiShare::handleCaptiveRedirect() {
    _server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    _server.sendHeader("Pragma", "no-cache");
    _server.sendHeader("Location", "http://192.168.4.1/", true);
    _server.send(302, "text/plain", "");
}

void WifiShare::handleNoContent() {
    _server.send(204, "text/plain", "");
}

void WifiShare::handleNotFound() {
    handleCaptiveRedirect();
}
