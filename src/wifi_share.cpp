#include "wifi_share.h"
#include "sd_storage.h"
#include "camera_driver.h"
#include "audio_mic.h"
#include "esp_log.h"
#include <SD.h>

static const char* TAG = "WIFI_SHARE";

WebServer WifiShare::_server(80);
DNSServer WifiShare::_dnsServer;
bool WifiShare::_running = false;

bool WifiShare::start() {
    ESP_LOGI(TAG, "Starting SoftAP: %s (Open Wi-Fi)...", WIFI_AP_SSID);

    WiFi.mode(WIFI_AP);
    const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : NULL;
    bool apOk = WiFi.softAP(WIFI_AP_SSID, pass, WIFI_AP_CHANNEL);
    if (!apOk) {
        ESP_LOGE(TAG, "Failed to start SoftAP!");
        return false;
    }

    IPAddress ip = WiFi.softAPIP();
    ESP_LOGI(TAG, "SoftAP started! IP Address: %s", ip.toString().c_str());

    // 1. Initialize mDNS responder (accessible at http://miphoto.local)
    if (MDNS.begin(WIFI_MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        ESP_LOGI(TAG, "mDNS responder started: http://%s.local", WIFI_MDNS_HOST);
    }

    // 2. Initialize Captive Portal DNS on port 53 (redirects any domain to camera IP)
    _dnsServer.start(53, "*", ip);
    ESP_LOGI(TAG, "DNS Captive Portal started on port 53");

    // Web server route registration
    _server.on("/", HTTP_GET, handleRoot);
    _server.on("/snapshot", HTTP_GET, handleSnapshot);
    _server.on("/capture", HTTP_POST, handleCapture);
    _server.on("/preset", HTTP_GET, handlePreset);
    _server.on("/audio_level", HTTP_GET, handleAudioLevel);
    _server.on("/toggle_clap", HTTP_POST, handleToggleClap);
    _server.on("/audio_threshold", HTTP_ANY, handleAudioThreshold);
    _server.on("/audio_gain", HTTP_ANY, handleAudioGain);
    _server.on("/download", HTTP_GET, handleDownload);
    _server.on("/view", HTTP_GET, handleView);
    _server.on("/delete", HTTP_POST, handleDelete);
    _server.onNotFound(handleNotFound);

    _server.begin();
    _running = true;
    CameraDriver::setModeWifiStream();  // Fast QVGA JPEG preview for Wi-Fi stream
    ESP_LOGI(TAG, "HTTP Server started on port 80");
    return true;
}

void WifiShare::stop() {
    if (_running) {
        _dnsServer.stop();
        MDNS.end();
        _server.stop();
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
    // Fast preview frame for mobile browser
    camera_fb_t* fb = CameraDriver::getFrame();
    if (!fb) {
        _server.send(503, "text/plain", "Camera Busy");
        return;
    }

    if (fb->format == PIXFORMAT_JPEG) {
        _server.send_P(200, "image/jpeg", reinterpret_cast<const char*>(fb->buf), fb->len);
    } else {
        _server.send(500, "text/plain", "Frame not JPEG");
    }
    CameraDriver::returnFrame(fb);
}

void WifiShare::handleCapture() {
    ESP_LOGI(TAG, "Remote capture triggered via Web!");

    // Capture full-resolution UXGA frame
    CameraDriver::setModeCapture();
    camera_fb_t* fb = CameraDriver::getFrame();
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        if (fb) CameraDriver::returnFrame(fb);
        CameraDriver::setModeWifiStream();
        _server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Capture failed\"}");
        return;
    }

    char savedFilename[32];
    bool ok = SdStorage::savePhoto(fb->buf, fb->len, savedFilename, sizeof(savedFilename));
    CameraDriver::returnFrame(fb);
    CameraDriver::setModeWifiStream();  // Return to smooth streaming preview

    if (ok) {
        _server.send(200, "application/json", "{\"status\":\"ok\",\"file\":\"" + String(savedFilename) + "\"}");
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

    String json = "{\"level\":" + String(lvl) + ",\"peak\":" + String(peak) + ",\"threshold\":" + String(thresh) +
                  ",\"gain\":" + String(gain) + ",\"clap_enabled\":" + (clap ? "true" : "false") +
                  ",\"countdown\":" + (countdown ? "true" : "false") + ",\"remaining_ms\":" + String(rem) + "}";
    _server.send(200, "application/json", json);
}

void WifiShare::handleToggleClap() {
    bool current = AudioMic::isClapTriggerEnabled();
    AudioMic::setClapTriggerEnabled(!current);
    _server.send(200, "text/plain", !current ? "ENABLED" : "DISABLED");
}

void WifiShare::handleAudioThreshold() {
    if (_server.hasArg("val")) {
        int val = _server.arg("val").toInt();
        if (val >= 20 && val <= 900) {
            AudioMic::setThreshold((uint16_t)val);
            _server.send(200, "text/plain", String(val));
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
            _server.send(200, "text/plain", String(val));
            return;
        }
    }
    _server.send(400, "text/plain", "Invalid gain (1-10)");
}

void WifiShare::handleRoot() {
    std::vector<String> files = SdStorage::getPhotoList();
    uint64_t totalMb = SdStorage::getTotalBytes() / (1024 * 1024);
    uint64_t usedMb = SdStorage::getUsedBytes() / (1024 * 1024);

    String html =
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>MIPHOTO // CONTROL CONSOLE</title>"
        "<style>"
        ":root {"
        "  --bg: #0c0e12; --chassis: #14171e; --border: #262c38; --border-bright: #3d4659;"
        "  --text: #e2e8f0; --muted: #798599; --accent: #f59e0b; --red: #dc2626; --green: #10b981;"
        "}"
        "* { box-sizing: border-box; margin: 0; padding: 0; }"
        "body { font-family: ui-monospace, 'SF Mono', Menlo, Consolas, monospace; background: var(--bg); color: "
        "var(--text); padding: 16px; font-size: 13px; -webkit-font-smoothing: antialiased; }"
        ".container { max-width: 640px; margin: 0 auto; }"
        "header { border: 1px solid var(--border); background: var(--chassis); padding: 12px 16px; margin-bottom: "
        "14px; display: flex; justify-content: space-between; align-items: center; }"
        ".brand { font-weight: 700; letter-spacing: 0.1em; color: var(--accent); font-size: 14px; }"
        ".telemetry { font-size: 11px; color: var(--muted); text-align: right; line-height: 1.4; }"
        ".deck { border: 1px solid var(--border); background: var(--chassis); padding: 16px; margin-bottom: 16px; }"
        ".deck-title { font-size: 11px; text-transform: uppercase; letter-spacing: 0.1em; color: var(--muted); "
        "border-bottom: 1px solid var(--border); padding-bottom: 8px; margin-bottom: 12px; display: flex; "
        "justify-content: space-between; }"
        ".finder-frame { position: relative; background: #000; border: 1px solid var(--border-bright); overflow: "
        "hidden; margin-bottom: 14px; min-height: 240px; display: flex; align-items: center; justify-content: center; }"
        "#stream { width: 100%; max-height: 340px; object-fit: contain; display: block; }"
        ".reticle { position: absolute; inset: 12px; pointer-events: none; border: 1px dashed rgba(245, 158, 11, "
        "0.25); display: flex; flex-direction: column; justify-content: space-between; }"
        ".reticle-top { display: flex; justify-content: space-between; font-size: 10px; color: var(--accent); padding: "
        "4px; }"
        ".reticle-bottom { display: flex; justify-content: space-between; font-size: 10px; color: var(--muted); "
        "padding: 4px; }"
        ".controls-grid { display: grid; grid-template-columns: 2fr 1fr; gap: 10px; margin-bottom: 14px; }"
        ".btn-shutter { background: radial-gradient(circle at 35% 35%, #ef4444, #991b1b); color: #fff; border: 2px "
        "solid #7f1d1d; border-radius: 4px; padding: 16px; font-family: inherit; font-size: 13px; font-weight: 700; "
        "letter-spacing: 0.08em; text-transform: uppercase; cursor: pointer; box-shadow: 0 4px 12px rgba(185, 28, 28, "
        "0.35); transition: 0.05s; }"
        ".btn-shutter:active { transform: translateY(2px); box-shadow: inset 0 3px 6px rgba(0,0,0,0.6); }"
        ".btn-timer { background: #1a202c; color: #cbd5e1; border: 1px solid var(--border-bright); border-radius: 4px; "
        "padding: 16px; font-family: inherit; font-size: 12px; font-weight: 700; letter-spacing: 0.05em; "
        "text-transform: uppercase; cursor: pointer; transition: 0.05s; }"
        ".btn-timer:active { background: #2d3748; transform: translateY(1px); }"
        ".preset-bank { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-bottom: 14px; }"
        ".btn-preset { background: #0f1217; color: var(--muted); border: 1px solid var(--border); border-radius: 3px; "
        "padding: 10px 4px; font-family: inherit; font-size: 11px; font-weight: 600; letter-spacing: 0.05em; cursor: "
        "pointer; text-align: center; transition: 0.1s; }"
        ".btn-preset.active { background: #1e2430; color: var(--accent); border-color: var(--accent); box-shadow: "
        "inset 0 0 0 1px var(--accent); }"
        ".vu-meter { background: #0f1217; border: 1px solid var(--border); padding: 12px; border-radius: 3px; "
        "transition: border-color 0.15s; }"
        ".vu-header { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted); "
        "margin-bottom: 8px; text-transform: uppercase; font-weight: 600; }"
        ".vu-track { position: relative; height: 16px; background: #090b0e; border: 1px solid var(--border); overflow: "
        "hidden; margin-bottom: 10px; border-radius: 2px; }"
        ".vu-bar { height: 100%; width: 0%; background: var(--green); transition: width 0.06s ease-out; }"
        ".vu-peak { position: absolute; top: 0; bottom: 0; width: 3px; background: #f59e0b; transition: left 0.08s; "
        "pointer-events: none; }"
        ".vu-thresh-line { position: absolute; top: 0; bottom: 0; width: 2px; background: #ef4444; z-index: 5; "
        "pointer-events: none; box-shadow: 0 0 5px #ef4444; }"
        ".vu-ctrl { display: flex; justify-content: space-between; align-items: center; gap: 8px; flex-wrap: wrap; }"
        ".vu-btn { background: #1a202c; color: #cbd5e1; border: 1px solid var(--border); padding: 6px 10px; "
        "font-family: inherit; font-size: 11px; font-weight: 700; cursor: pointer; text-transform: uppercase; "
        "border-radius: 3px; transition: 0.05s; }"
        ".vu-btn:active { background: #2d3748; transform: translateY(1px); }"
        ".film-strip { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 12px; }"
        ".frame-card { background: #090b0e; border: 1px solid var(--border); padding: 8px; }"
        ".frame-img { width: 100%; height: 200px; object-fit: cover; background: #000; border: 1px solid #1c212b; "
        "display: block; }"
        ".frame-meta { padding: 8px 2px 4px 2px; display: flex; justify-content: space-between; align-items: center; "
        "font-size: 11px; }"
        ".frame-name { color: #cbd5e1; font-weight: 600; }"
        ".frame-size { color: var(--muted); font-size: 10px; }"
        ".frame-actions { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; margin-top: 6px; }"
        ".btn-act { padding: 7px; text-align: center; text-decoration: none; font-size: 11px; font-weight: 600; "
        "letter-spacing: 0.05em; border: 1px solid var(--border); border-radius: 3px; cursor: pointer; text-transform: "
        "uppercase; font-family: inherit; }"
        ".btn-down { background: #13271e; color: #34d399; border-color: #065f46; }"
        ".btn-down:hover { background: #163628; }"
        ".btn-del { background: #261416; color: #f87171; border-color: #7f1d1d; }"
        ".btn-del:hover { background: #381a1d; }"
        "</style>"
        "<script>"
        "let streamRunning = true;"
        "let currentThreshold = 160;"
        "let currentGain = 3;"
        "function streamNext() {"
        "  if (!streamRunning) return;"
        "  setTimeout(() => {"
        "    document.getElementById('stream').src = '/snapshot?t=' + Date.now();"
        "  }, 60);"
        "}"
        "function streamRetry() {"
        "  setTimeout(() => {"
        "    document.getElementById('stream').src = '/snapshot?t=' + Date.now();"
        "  }, 250);"
        "}"
        "function shoot() {"
        "  streamRunning = false;"
        "  let b = document.getElementById('shutter-btn');"
        "  b.innerText = '[ EXPOSING UXGA... ]';"
        "  fetch('/capture', {method: 'POST'}).then(r => r.json()).then(d => {"
        "    alert(d.status == 'ok' ? 'FRAME COMMITTED: ' + d.file : 'CAPTURE ERROR');"
        "    b.innerText = 'SHUTTER // CAPTURE UXGA';"
        "    streamRunning = true;"
        "    setTimeout(() => location.reload(), 300);"
        "  }).catch(() => {"
        "    b.innerText = 'SHUTTER // CAPTURE UXGA';"
        "    streamRunning = true;"
        "  });"
        "}"
        "function shootTimer() {"
        "  let cnt = 2;"
        "  let btn = document.getElementById('timer-btn');"
        "  btn.disabled = true;"
        "  btn.innerText = '[ ' + cnt + 's COUNTDOWN ]';"
        "  let iv = setInterval(() => {"
        "    cnt--;"
        "    if (cnt > 0) {"
        "      btn.innerText = '[ ' + cnt + 's COUNTDOWN ]';"
        "    } else {"
        "      clearInterval(iv);"
        "      btn.innerText = '[ 2s TIMER ]';"
        "      btn.disabled = false;"
        "      shoot();"
        "    }"
        "  }, 1000);"
        "}"
        "function setPreset(id, el) {"
        "  fetch('/preset?id=' + id).then(r => {"
        "    document.querySelectorAll('.btn-preset').forEach(b => b.classList.remove('active'));"
        "    el.classList.add('active');"
        "  });"
        "}"
        "function adjThresh(delta) {"
        "  let nv = Math.max(20, Math.min(800, currentThreshold + delta));"
        "  fetch('/audio_threshold?val=' + nv, {method: 'POST'}).then(() => {"
        "    currentThreshold = nv;"
        "    document.getElementById('thresh-val').innerText = nv;"
        "    document.getElementById('mic-thresh-line').style.left = Math.min(100, Math.round(nv / 10)) + '%';"
        "  });"
        "}"
        "function cycleGain() {"
        "  currentGain = (currentGain % 5) + 1;"
        "  fetch('/audio_gain?val=' + currentGain, {method: 'POST'}).then(() => {"
        "    document.getElementById('gain-btn').innerText = 'GAIN: ' + currentGain + 'x';"
        "  });"
        "}"
        "function toggleClapTrigger() {"
        "  fetch('/toggle_clap', {method: 'POST'}).then(r => r.text()).then(txt => {"
        "    let b = document.getElementById('clap-tog-btn');"
        "    let on = (txt.indexOf('ENABLED') >= 0);"
        "    b.innerText = 'TRIGGER: ' + (on ? 'ON' : 'OFF');"
        "    b.style.color = on ? '#34d399' : '#f87171';"
        "  });"
        "}"
        "function deleteFile(path) {"
        "  if(confirm('PURGE FRAME: ' + path + '?')) {"
        "    fetch('/delete?file=' + encodeURIComponent(path), {method: 'POST'}).then(() => location.reload());"
        "  }"
        "}"
        "let audioCtx = null;"
        "function getAudioCtx() {"
        "  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();"
        "  return audioCtx;"
        "}"
        "function playSimTone(freq, durMs, delayMs = 0) {"
        "  let ctx = getAudioCtx();"
        "  let osc = ctx.createOscillator();"
        "  let gain = ctx.createGain();"
        "  osc.type = 'square';"
        "  let st = ctx.currentTime + (delayMs / 1000);"
        "  let et = st + (durMs / 1000);"
        "  osc.frequency.setValueAtTime(freq, st);"
        "  gain.gain.setValueAtTime(0.06, st);"
        "  gain.gain.setValueAtTime(0.001, et);"
        "  osc.connect(gain); gain.connect(ctx.destination);"
        "  osc.start(st); osc.stop(et);"
        "}"
        "function simMiiGreeting() {"
        "  playSimTone(880, 80, 0);"
        "  playSimTone(1175, 140, 105);"
        "}"
        "function simShutter() {"
        "  playSimTone(2200, 15, 0);"
        "  playSimTone(850, 20, 25);"
        "}"
        "function simMiiTheme() {"
        "  let notes = "
        "[[370,180],[440,180],[554,240],[0,40],[440,180],[370,180],[294,200],[0,60],[294,120],[0,40],[294,180]];"
        "  let t = 0;"
        "  for (let n of notes) {"
        "    if (n[0] > 0) playSimTone(n[0], n[1], t);"
        "    t += n[1];"
        "  }"
        "}"
        "setInterval(() => {"
        "  fetch('/audio_level').then(r => r.json()).then(d => {"
        "    let pct = Math.min(100, Math.round(d.level / 10));"
        "    let peakPct = Math.min(100, Math.round(d.peak / 10));"
        "    let threshPct = Math.min(100, Math.round(d.threshold / 10));"
        "    currentThreshold = d.threshold;"
        "    currentGain = d.gain || 3;"
        "    let bar = document.getElementById('mic-bar');"
        "    let peakEl = document.getElementById('mic-peak');"
        "    let thLine = document.getElementById('mic-thresh-line');"
        "    let txt = document.getElementById('mic-val');"
        "    let thVal = document.getElementById('thresh-val');"
        "    let gainBtn = document.getElementById('gain-btn');"
        "    let togBtn = document.getElementById('clap-tog-btn');"
        "    let cb = document.getElementById('countdown-banner');"
        "    let cs = document.getElementById('countdown-sec');"
        "    let vBox = document.getElementById('vu-box');"
        "    if (bar) bar.style.width = pct + '%';"
        "    if (peakEl) peakEl.style.left = peakPct + '%';"
        "    if (thLine) thLine.style.left = threshPct + '%';"
        "    if (thVal) thVal.innerText = d.threshold;"
        "    if (gainBtn) gainBtn.innerText = 'GAIN: ' + currentGain + 'x';"
        "    if (togBtn) {"
        "      togBtn.innerText = 'TRIGGER: ' + (d.clap_enabled ? 'ON' : 'OFF');"
        "      togBtn.style.color = d.clap_enabled ? '#34d399' : '#f87171';"
        "    }"
        "    if (txt) txt.innerText = 'LVL: ' + pct + '% | PEAK: ' + peakPct + '% | THR: ' + threshPct + '%';"
        "    if (d.countdown && d.remaining_ms > 0) {"
        "      if (cb) cb.style.display = 'block';"
        "      if (cs) cs.innerText = (d.remaining_ms / 1000).toFixed(1);"
        "      if (vBox) vBox.style.borderColor = '#ef4444';"
        "      if (bar) bar.style.background = '#dc2626';"
        "    } else {"
        "      if (cb) cb.style.display = 'none';"
        "      if (vBox) vBox.style.borderColor = 'var(--border)';"
        "      if (bar) bar.style.background = (d.level >= d.threshold) ? '#dc2626' : 'var(--green)';"
        "    }"
        "  }).catch(() => {});"
        "}, 150);"
        "</script></head><body>"
        "<div class='container'>"
        "<header>"
        "<div class='brand'>MIPHOTO</div>"
        "<div class='telemetry'>"
        "HOST: http://miphoto.local // IP: 192.168.4.1<br>"
        "SD STORAGE: " +
        String((int)usedMb) + " MB / " + String((int)totalMb) + " MB [" + String((int)files.size()) +
        " FRAMES]"
        "</div>"
        "</header>"
        "<div class='deck'>"
        "<div class='deck-title'><span>01 // OPTICAL VIEWFINDER</span><span>STREAM: 15-20 FPS</span></div>"
        "<div class='finder-frame'>"
        "<img id='stream' src='/snapshot' alt='Live Preview' onload='streamNext()' onerror='streamRetry()'>"
        "<div class='reticle'>"
        "<div class='reticle-top'><span>[+] UXGA 1600x1200</span><span>● LIVE</span></div>"
        "<div class='reticle-bottom'><span>F/2.8 // RAW GAMMA</span><span>OV2640 DSP</span></div>"
        "</div>"
        "</div>"
        "<div class='controls-grid'>"
        "<button id='shutter-btn' class='btn-shutter' onclick='shoot()'>SHUTTER // CAPTURE UXGA</button>"
        "<button id='timer-btn' class='btn-timer' onclick='shootTimer()'>[ 2s TIMER ]</button>"
        "</div>"
        "<div class='deck-title' style='margin-top:16px;'><span>02 // ISP EMULATION MATRIX</span><span>HARDWARE "
        "REGISTERS</span></div>"
        "<div class='preset-bank'>"
        "<button class='btn-preset active' onclick='setPreset(0, this)'>[ AUTO ISP ]</button>"
        "<button class='btn-preset' onclick='setPreset(1, this)'>[ SUNNY DAY ]</button>"
        "<button class='btn-preset' onclick='setPreset(2, this)'>[ NIGHT BOOST ]</button>"
        "<button class='btn-preset' onclick='setPreset(3, this)'>[ PORTRAIT ]</button>"
        "<button class='btn-preset' onclick='setPreset(4, this)'>[ Y2K RETRO ]</button>"
        "<button class='btn-preset' onclick='setPreset(5, this)'>[ LOMO CROSS ]</button>"
        "</div>"
        "<div class='vu-meter' id='vu-box'>"
        "<div class='vu-header'><span>03 // PDM ACOUSTIC SENSOR (CLAP SHUTTER)</span><span id='mic-val'>LVL: 0% | "
        "PEAK: 0%</span></div>"
        "<div class='vu-track'>"
        "<div id='mic-bar' class='vu-bar'></div>"
        "<div id='mic-peak' class='vu-peak' style='left:0%;'></div>"
        "<div id='mic-thresh-line' class='vu-thresh-line' style='left:16%;'></div>"
        "</div>"
        "<div class='vu-ctrl'>"
        "<div style='display:flex; align-items:center; gap:6px;'>"
        "<span style='color:var(--muted); font-size:10px;'>THRESH:</span>"
        "<button class='vu-btn' onclick='adjThresh(-20)'>[-20]</button>"
        "<span id='thresh-val' style='color:#ef4444; font-weight:700; min-width:32px; text-align:center;'>160</span>"
        "<button class='vu-btn' onclick='adjThresh(20)'>[+20]</button>"
        "</div>"
        "<div style='display:flex; align-items:center; gap:6px;'>"
        "<button id='gain-btn' class='vu-btn' onclick='cycleGain()'>GAIN: 3x</button>"
        "<button id='clap-tog-btn' class='vu-btn' onclick='toggleClapTrigger()'>TRIGGER: ON</button>"
        "</div>"
        "</div>"
        "<div id='countdown-banner' style='display:none; margin-top:8px; padding:6px 10px; background:#7f1d1d; "
        "color:#fff; font-weight:700; font-size:12px; text-align:center; border:1px solid #dc2626; border-radius:2px;'>"
        "[!] CLAP DETECTED — EXPOSING IN <span id='countdown-sec'>2.0</span>s"
        "</div>"
        "</div>"
        "<div class='deck-title' style='margin-top:16px;'><span>04 // PIEZO AUDIO SYNTHESIZER (D5 / GPIO "
        "6)</span><span>8-BIT SQUARE WAVE</span></div>"
        "<div style='display:grid; grid-template-columns: repeat(3, 1fr); gap:6px;'>"
        "<button class='btn-preset' onclick='simMiiGreeting()'>[ SIM: \"MII-II!\" ]</button>"
        "<button class='btn-preset' onclick='simShutter()'>[ SIM: SHUTTER ]</button>"
        "<button class='btn-preset' onclick='simMiiTheme()'>[ SIM: MII THEME ]</button>"
        "</div>"
        "</div>"
        "<div class='deck'>"
        "<div class='deck-title'><span>05 // STORAGE ARCHIVE (/DCIM)</span><span>FAT32 REPOSITORY</span></div>";

    if (files.empty()) {
        html +=
            "<div style='padding:24px; text-align:center; color:var(--muted); font-size:12px;'>ARCHIVE EMPTY. TRIGGER "
            "SHUTTER TO COMMIT FIRST FRAME.</div>";
    } else {
        html += "<div class='film-strip'>";
        for (int i = files.size() - 1; i >= 0; i--) {
            String path = files[i];
            size_t sz = SdStorage::getPhotoSize(path.c_str());
            String sizeKb = String((float)sz / 1024.0f, 1) + " KB";
            String filename = path.substring(path.lastIndexOf('/') + 1);
            html += "<div class='frame-card'>";
            html += "<img src='/view?file=" + path + "' class='frame-img' loading='lazy' alt='" + filename + "'>";
            html += "<div class='frame-meta'>";
            html += "<span class='frame-name'>" + filename + "</span>";
            html += "<span class='frame-size'>" + sizeKb + " // UXGA</span>";
            html += "</div>";
            html += "<div class='frame-actions'>";
            html += "<a href='/download?file=" + path + "' class='btn-act btn-down'>[ GET ]</a>";
            html += "<button onclick=\"deleteFile('" + path + "')\" class='btn-act btn-del'>[ PURGE ]</button>";
            html += "</div></div>";
        }
        html += "</div>";
    }

    html += "</div></div></body></html>";
    _server.send(200, "text/html", html);
}

void WifiShare::handleDownload() {
    if (!_server.hasArg("file")) {
        _server.send(400, "text/plain", "Missing file parameter");
        return;
    }

    String path = _server.arg("file");
    if (!SD.exists(path)) {
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
    if (!SD.exists(path)) {
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
    if (SdStorage::deletePhoto(path.c_str())) {
        _server.send(200, "text/plain", "Deleted");
    } else {
        _server.send(500, "text/plain", "Delete failed");
    }
}

void WifiShare::handleNotFound() {
    // Captive Portal redirect:
    // Navigating to 'miphoto', 'miphoto.local', or any domain in the browser
    // automatically redirects to the camera control console.
    _server.sendHeader("Location", "http://192.168.4.1/", true);
    _server.send(302, "text/plain", "Redirecting to MIphoto...");
}
