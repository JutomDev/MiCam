#pragma once

#include <pgmspace.h>

#ifndef PROGMEM
#define PROGMEM
#endif

// ============================================================================
// PROGMEM HTML/CSS/JS TEMPLATES (Zero Internal SRAM Heap Fragmentation)
// ============================================================================
static const char HTML_SHARED_STYLE[] PROGMEM = R"rawliteral(<style>
:root {
  --bg: #f3f0ea;
  --surface: #ffffff;
  --surface-soft: #ebe6dc;
  --surface-hover: #e4ded2;
  --border: #dfd9cd;
  --border-strong: #cbc3b4;
  --text: #1f2328;
  --muted: #6c665d;
  --shutter: #dc4c36;
  --shutter-dark: #ba3824;
  --accent: #b45309;
  --accent-bg: #fef3c7;
  --green: #227858;
  --green-bg: #e7f4ee;
  --red: #c2392b;
  --red-bg: #fdeceb;
  --radius: 12px;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  padding: 14px;
  font-size: 14px;
  line-height: 1.4;
  -webkit-font-smoothing: antialiased;
}
.mono { font-family: ui-monospace, 'SF Mono', Menlo, Consolas, monospace; }
.container { max-width: 720px; margin: 0 auto; }

/* Header */
header {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 12px 16px;
  margin-bottom: 10px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
  box-shadow: 0 1px 3px rgba(31, 35, 40, 0.04);
}
.brand-wrap { display: flex; align-items: center; gap: 8px; }
.brand-dot { width: 9px; height: 9px; border-radius: 50%; background: var(--shutter); flex-shrink: 0; }
.brand { font-weight: 700; letter-spacing: -0.01em; font-size: 16px; color: var(--text); }
.telemetry { font-size: 11px; color: var(--muted); text-align: right; line-height: 1.35; }
.telemetry strong { color: var(--text); font-weight: 600; }

/* Navigation Tabs */
.tabs {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 6px;
  background: var(--surface-soft);
  padding: 4px;
  border-radius: var(--radius);
  border: 1px solid var(--border);
  margin-bottom: 14px;
}
.tab {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 7px;
  text-decoration: none;
  padding: 9px 12px;
  border-radius: 9px;
  font-weight: 600;
  font-size: 13px;
  color: var(--muted);
  transition: background 0.15s, color 0.15s;
}
.tab.active {
  background: var(--surface);
  color: var(--text);
  box-shadow: 0 1px 2px rgba(31, 35, 40, 0.08);
}
.tab:not(.active):hover { color: var(--text); background: rgba(255, 255, 255, 0.45); }
.tab-badge {
  background: var(--surface-soft);
  color: var(--text);
  font-size: 11px;
  padding: 1px 7px;
  border-radius: 99px;
  font-weight: 600;
}

/* Cards */
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 14px;
  margin-bottom: 12px;
  box-shadow: 0 1px 3px rgba(31, 35, 40, 0.04);
}
.card-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
  margin-bottom: 10px;
}
.card-title { font-size: 13px; font-weight: 700; color: var(--text); letter-spacing: 0.01em; }
.card-sub { font-size: 11px; color: var(--muted); }

/* Viewfinder */
.finder-frame {
  position: relative;
  background: #181a1d;
  border-radius: 9px;
  overflow: hidden;
  margin-bottom: 12px;
  aspect-ratio: 4 / 3;
  display: flex;
  align-items: center;
  justify-content: center;
}
#stream { width: 100%; height: 100%; object-fit: contain; display: block; }
.finder-pill {
  position: absolute;
  top: 10px;
  left: 10px;
  background: rgba(20, 22, 25, 0.72);
  color: #f8fafc;
  font-size: 11px;
  font-weight: 600;
  padding: 3px 8px;
  border-radius: 99px;
  display: flex;
  align-items: center;
  gap: 5px;
  backdrop-filter: blur(4px);
}
.live-dot { width: 6px; height: 6px; border-radius: 50%; background: #34d399; }
.finder-res {
  position: absolute;
  top: 10px;
  right: 10px;
  background: rgba(20, 22, 25, 0.65);
  color: #e2e8f0;
  font-size: 10px;
  padding: 3px 7px;
  border-radius: 99px;
}

/* Shutter Controls */
.controls-grid { display: grid; grid-template-columns: 2fr 1fr; gap: 8px; }
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 7px;
  font-family: inherit;
  font-weight: 600;
  font-size: 13px;
  border-radius: 9px;
  padding: 12px 14px;
  cursor: pointer;
  border: 1px solid transparent;
  transition: transform 0.06s, background 0.12s, border-color 0.12s;
  text-decoration: none;
}
.btn:active { transform: translateY(1px); }
.btn-shutter {
  background: var(--shutter);
  color: #fff;
  border-color: var(--shutter-dark);
  box-shadow: 0 2px 6px rgba(220, 76, 54, 0.28);
}
.btn-shutter:hover { background: #cf432e; }
.btn-soft {
  background: var(--surface-soft);
  color: var(--text);
  border-color: var(--border);
}
.btn-soft:hover { background: var(--surface-hover); }

/* Presets */
.preset-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 7px; }
@media (max-width: 400px) { .preset-grid { grid-template-columns: repeat(2, 1fr); } }
.btn-preset {
  background: var(--bg);
  color: var(--muted);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 9px 6px;
  font-family: inherit;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  text-align: center;
  transition: all 0.12s;
}
.btn-preset:hover { color: var(--text); border-color: var(--border-strong); }
.btn-preset.active {
  background: var(--accent-bg);
  color: var(--accent);
  border-color: #f59e0b;
}

/* Audio & Controls */
.vu-track {
  position: relative;
  height: 12px;
  background: var(--surface-soft);
  border: 1px solid var(--border);
  border-radius: 99px;
  overflow: hidden;
  margin-bottom: 10px;
}
.vu-bar { height: 100%; width: 0%; background: var(--green); border-radius: 99px; transition: width 0.07s ease-out; }
.vu-peak { position: absolute; top: 0; bottom: 0; width: 3px; background: #d97706; transition: left 0.08s; pointer-events: none; }
.vu-thresh-line { position: absolute; top: 0; bottom: 0; width: 2px; background: var(--red); z-index: 5; pointer-events: none; }
.audio-row { display: flex; justify-content: space-between; align-items: center; gap: 8px; flex-wrap: wrap; }
.ctrl-group { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
.chip-btn {
  background: var(--surface-soft);
  color: var(--text);
  border: 1px solid var(--border);
  padding: 6px 10px;
  font-family: inherit;
  font-size: 12px;
  font-weight: 600;
  border-radius: 7px;
  cursor: pointer;
}
.chip-btn:hover { background: var(--surface-hover); }
.chip-btn:active { transform: translateY(1px); }
.chip-on { background: var(--green-bg); color: var(--green); border-color: #b7e0cf; }
.chip-off { background: var(--red-bg); color: var(--red); border-color: #f3c4bf; }
.chimes-row {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  flex-wrap: wrap;
}

/* Gallery Grid (Rock-solid 2-col mobile / 3-col desktop, zero overflow) */
.gallery-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 9px;
  align-items: start;
}
@media (min-width: 540px) {
  .gallery-grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 12px;
  }
}
.frame-card {
  min-width: 0;
  overflow: hidden;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 7px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.frame-thumb {
  position: relative;
  width: 100%;
  aspect-ratio: 4 / 3;
  background: var(--surface-soft);
  border-radius: 7px;
  overflow: hidden;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: zoom-in;
}
.frame-placeholder { font-size: 11px; color: var(--muted); pointer-events: none; }
.frame-img {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
  opacity: 0;
  transition: opacity 0.2s ease;
}
.frame-img.loaded { opacity: 1; }
.frame-meta {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 4px;
  min-width: 0;
}
.frame-name {
  font-size: 11px;
  font-weight: 600;
  color: var(--text);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.frame-size { font-size: 10px; color: var(--muted); flex-shrink: 0; }
.frame-actions { display: grid; grid-template-columns: 1.35fr 1fr; gap: 5px; }
.btn-down {
  background: var(--green-bg);
  color: var(--green);
  border: 1px solid #b7e0cf;
  padding: 7px 4px;
  font-size: 11px;
  border-radius: 7px;
  white-space: nowrap;
}
.btn-down:hover { background: #d6ede2; }
.btn-del {
  background: var(--red-bg);
  color: var(--red);
  border: 1px solid #f3c4bf;
  padding: 7px 4px;
  font-size: 11px;
  border-radius: 7px;
  white-space: nowrap;
}
.btn-del:hover { background: #fadbd8; }
.btn-del.confirming { background: var(--red); color: #fff; border-color: var(--red); }

/* Tap-to-Zoom Lightbox */
.lightbox {
  position: fixed;
  inset: 0;
  background: rgba(18, 20, 23, 0.92);
  display: none;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 16px;
  z-index: 100;
}
.lightbox.open { display: flex; }
.lightbox img {
  max-width: 100%;
  max-height: 75vh;
  border-radius: 8px;
  object-fit: contain;
  background: #000;
}
.lightbox-bar {
  margin-top: 12px;
  display: flex;
  gap: 10px;
  align-items: center;
}
.lightbox-name { color: #e2e8f0; font-size: 12px; }

/* Pagination */
.pager {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
  margin-top: 14px;
  padding-top: 12px;
  border-top: 1px solid var(--border);
}
.pager-btn {
  padding: 8px 13px;
  border: 1px solid var(--border);
  background: var(--surface-soft);
  color: var(--text);
  text-decoration: none;
  font-size: 12px;
  font-weight: 600;
  border-radius: 8px;
}
.pager-btn:hover { background: var(--surface-hover); }
.pager-btn.disabled { opacity: 0.4; pointer-events: none; }
</style>)rawliteral";

static const char HTML_HEAD_SCRIPTS[] PROGMEM = R"rawliteral(<script>
let streamRunning = true;
let currentThreshold = 350;
let currentGain = 2;
let frameTotal = 0;
const PRESET_NAMES = ['Auto', 'Sunny', 'Night 5x', 'Portrait', 'Retro Y2K', 'Lomo'];
function streamNext() {
  if (!streamRunning) return;
  setTimeout(() => {
    let el = document.getElementById('stream');
    if (el) el.src = '/snapshot?t=' + Date.now();
  }, 110);
}
function streamRetry() {
  if (!streamRunning) return;
  setTimeout(() => {
    let el = document.getElementById('stream');
    if (el) el.src = '/snapshot?t=' + Date.now();
  }, 400);
}
function shoot() {
  streamRunning = false;
  let b = document.getElementById('shutter-btn');
  b.innerText = 'Capturing 2MP...';
  fetch('/capture', {method: 'POST'}).then(r => r.json()).then(d => {
    if (d.status == 'ok') {
      b.innerText = 'Saved ' + d.file;
      frameTotal++;
      let gc = document.getElementById('gal-cnt');
      if (gc) gc.innerText = frameTotal;
      let hc = document.getElementById('hdr-cnt');
      if (hc) hc.innerText = frameTotal;
    } else {
      b.innerText = 'Capture Error';
    }
    streamRunning = true;
    streamNext();
    setTimeout(() => { b.innerText = 'Take Photo'; }, 2000);
  }).catch(() => {
    b.innerText = 'Take Photo';
    streamRunning = true;
    streamNext();
  });
}
function shootTimer() {
  let cnt = 2;
  let btn = document.getElementById('timer-btn');
  btn.disabled = true;
  btn.innerText = cnt + 's...';
  let iv = setInterval(() => {
    cnt--;
    if (cnt > 0) {
      btn.innerText = cnt + 's...';
    } else {
      clearInterval(iv);
      btn.innerText = '2s Timer';
      btn.disabled = false;
      shoot();
    }
  }, 1000);
}
function setPreset(id, el) {
  fetch('/preset?id=' + id).then(() => {
    document.querySelectorAll('.btn-preset').forEach(b => b.classList.remove('active'));
    el.classList.add('active');
    let pb = document.getElementById('preset-badge');
    if (pb && PRESET_NAMES[id]) pb.innerText = PRESET_NAMES[id];
  });
}
function adjThresh(delta) {
  let nv = Math.max(20, Math.min(800, currentThreshold + delta));
  fetch('/audio_threshold?val=' + nv, {method: 'POST'}).then(() => {
    currentThreshold = nv;
    document.getElementById('thresh-val').innerText = nv;
    document.getElementById('mic-thresh-line').style.left = Math.min(100, Math.round(nv / 10)) + '%';
  });
}
function cycleGain() {
  currentGain = (currentGain % 5) + 1;
  fetch('/audio_gain?val=' + currentGain, {method: 'POST'}).then(() => {
    document.getElementById('gain-btn').innerText = 'Gain ' + currentGain + 'x';
  });
}
function updateToggleBtn(el, on, onText, offText) {
  if (!el) return;
  el.innerText = on ? onText : offText;
  el.classList.toggle('chip-on', on);
  el.classList.toggle('chip-off', !on);
}
function toggleClapTrigger() {
  fetch('/toggle_clap', {method: 'POST'}).then(r => r.text()).then(txt => {
    updateToggleBtn(document.getElementById('clap-tog-btn'), txt.indexOf('ENABLED') >= 0, 'Clap: On', 'Clap: Off');
  });
}
function toggleSpeakerMute() {
  fetch('/toggle_mute', {method: 'POST'}).then(r => r.text()).then(txt => {
    let muted = (txt.indexOf('MUTED') >= 0);
    updateToggleBtn(document.getElementById('mute-btn'), !muted, 'Sound: On', 'Sound: Muted');
  });
}
let audioCtx = null;
function getAudioCtx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return audioCtx;
}
function playSimTone(freq, durMs, delayMs = 0) {
  let ctx = getAudioCtx();
  let osc = ctx.createOscillator();
  let gain = ctx.createGain();
  osc.type = 'triangle';
  let st = ctx.currentTime + (delayMs / 1000);
  let et = st + (durMs / 1000);
  osc.frequency.setValueAtTime(freq, st);
  gain.gain.setValueAtTime(0.08, st);
  gain.gain.setValueAtTime(0.001, et);
  osc.connect(gain); gain.connect(ctx.destination);
  osc.start(st); osc.stop(et);
}
function simMiiGreeting() { playSimTone(880, 80, 0); playSimTone(1175, 140, 105); }
function simShutter() { playSimTone(2200, 15, 0); playSimTone(850, 20, 25); }
function simMiiTheme() {
  let notes = [[370,180],[440,180],[554,240],[0,40],[440,180],[370,180],[294,200],[0,60],[294,120],[0,40],[294,180]];
  let t = 0;
  for (let n of notes) {
    if (n[0] > 0) playSimTone(n[0], n[1], t);
    t += n[1];
  }
}
setInterval(() => {
  fetch('/audio_level').then(r => r.json()).then(d => {
    let pct = Math.min(100, Math.round(d.level / 10));
    let peakPct = Math.min(100, Math.round(d.peak / 10));
    let threshPct = Math.min(100, Math.round(d.threshold / 10));
    currentThreshold = d.threshold;
    currentGain = d.gain || 2;
    let bar = document.getElementById('mic-bar');
    let peakEl = document.getElementById('mic-peak');
    let thLine = document.getElementById('mic-thresh-line');
    let txt = document.getElementById('mic-val');
    let thVal = document.getElementById('thresh-val');
    let gainBtn = document.getElementById('gain-btn');
    let cb = document.getElementById('countdown-banner');
    let cs = document.getElementById('countdown-sec');
    if (bar) bar.style.width = pct + '%';
    if (peakEl) peakEl.style.left = peakPct + '%';
    if (thLine) thLine.style.left = threshPct + '%';
    if (thVal) thVal.innerText = d.threshold;
    if (gainBtn) gainBtn.innerText = 'Gain ' + currentGain + 'x';
    updateToggleBtn(document.getElementById('clap-tog-btn'), !!d.clap_enabled, 'Clap: On', 'Clap: Off');
    updateToggleBtn(document.getElementById('mute-btn'), !d.muted, 'Sound: On', 'Sound: Muted');
    if (txt) txt.innerText = 'Level ' + pct + '% · Peak ' + peakPct + '%';
    if (d.countdown && d.remaining_ms > 0) {
      if (cb) cb.style.display = 'block';
      if (cs) cs.innerText = (d.remaining_ms / 1000).toFixed(1);
      if (bar) bar.style.background = 'var(--red)';
    } else {
      if (cb) cb.style.display = 'none';
      if (bar) bar.style.background = (d.level >= d.threshold) ? 'var(--red)' : 'var(--green)';
    }
  }).catch(() => {});
}, 300);
</script></head><body><div class='container'>
)rawliteral";

static const char HTML_GALLERY_SCRIPTS[] PROGMEM = R"rawliteral(<script>
function handleDeleteClick(btn, path) {
  if (!btn.dataset.confirm) {
    btn.dataset.confirm = '1';
    btn.classList.add('confirming');
    btn.innerText = 'Sure?';
    setTimeout(() => {
      if (btn && btn.dataset.confirm) {
        delete btn.dataset.confirm;
        btn.classList.remove('confirming');
        btn.innerText = 'Del';
      }
    }, 3000);
    return;
  }
  btn.disabled = true;
  btn.innerText = '...';
  fetch('/delete?file=' + encodeURIComponent(path), {method: 'POST'}).then(() => location.reload());
}
function openPreview(src, dlUrl, name) {
  const lb = document.getElementById('lb');
  if (!lb) return;
  document.getElementById('lb-img').src = src;
  document.getElementById('lb-dl').href = dlUrl;
  document.getElementById('lb-name').innerText = name;
  lb.classList.add('open');
}
function closePreview() {
  const lb = document.getElementById('lb');
  if (lb) lb.classList.remove('open');
}
function loadSequentialThumbs() {
  const imgs = Array.from(document.querySelectorAll('img[data-src]'));
  let idx = 0;
  function next() {
    if (idx >= imgs.length) return;
    const img = imgs[idx++];
    const lbl = img.parentElement ? img.parentElement.querySelector('.frame-placeholder') : null;
    img.onload = () => {
      img.classList.add('loaded');
      if (lbl) lbl.style.display = 'none';
      next();
    };
    img.onerror = () => {
      if (lbl) lbl.innerText = 'Unavailable';
      next();
    };
    img.src = img.dataset.src;
  }
  next();
}
document.addEventListener('DOMContentLoaded', loadSequentialThumbs);
</script></head><body>
<div id='lb' class='lightbox' onclick='if(event.target===this)closePreview()'>
<img id='lb-img' src='' alt='Preview'>
<div class='lightbox-bar'>
<span id='lb-name' class='lightbox-name mono'></span>
<a id='lb-dl' href='#' class='btn btn-down' style='padding:8px 14px;'>Download</a>
<button class='btn btn-soft' style='padding:8px 14px;' onclick='closePreview()'>Close</button>
</div></div>
<div class='container'>
)rawliteral";

static const char HTML_DECK_FINDER[] PROGMEM = R"rawliteral(<div class='card'>
<div class='card-head'>
<span class='card-title'>Live Viewfinder</span>
<span class='card-sub mono'>640x480 Preview · 2MP Capture</span>
</div>
<div class='finder-frame'>
<img id='stream' src='/snapshot' alt='Live Preview' onload='streamNext()' onerror='streamRetry()'>
<div class='finder-pill'><span class='live-dot'></span><span id='preset-badge'>Auto</span></div>
<div class='finder-res mono'>OV2640</div>
</div>
<div class='controls-grid'>
<button id='shutter-btn' class='btn btn-shutter' onclick='shoot()'>Take Photo</button>
<button id='timer-btn' class='btn btn-soft' onclick='shootTimer()'>2s Timer</button>
</div>
</div>
<div class='card'>
<div class='card-head'>
<span class='card-title'>Color Profile</span>
<span class='card-sub'>Sensor ISP</span>
</div>
<div class='preset-grid'>
)rawliteral";

static const char HTML_DECK_AUDIO[] PROGMEM = R"rawliteral(</div></div>
<div class='card' id='vu-box'>
<div class='card-head'>
<span class='card-title'>Audio &amp; Clap Trigger</span>
<span id='mic-val' class='card-sub mono'>Level 0% · Peak 0%</span>
</div>
<div class='vu-track'>
<div id='mic-bar' class='vu-bar'></div>
<div id='mic-peak' class='vu-peak' style='left:0%;'></div>
<div id='mic-thresh-line' class='vu-thresh-line' style='left:35%;'></div>
</div>
<div class='audio-row'>
<div class='ctrl-group'>
<span class='card-sub'>Threshold</span>
<button class='chip-btn' onclick='adjThresh(-20)'>-</button>
<span id='thresh-val' class='mono' style='font-size:12px; font-weight:700; min-width:30px; text-align:center;'>350</span>
<button class='chip-btn' onclick='adjThresh(20)'>+</button>
<button id='gain-btn' class='chip-btn' onclick='cycleGain()'>Gain 2x</button>
</div>
<div class='ctrl-group'>
<button id='clap-tog-btn' class='chip-btn chip-off' onclick='toggleClapTrigger()'>Clap: Off</button>
)rawliteral";

static const char HTML_DECK_SIM_BTNS[] PROGMEM = R"rawliteral(</div></div>
<div id='countdown-banner' style='display:none; margin-top:10px; padding:8px 12px; background:var(--red-bg); color:var(--red); font-weight:700; font-size:12px; text-align:center; border:1px solid #f3c4bf; border-radius:8px;'>
Clap detected — capturing in <span id='countdown-sec'>2.0</span>s
</div>
<div class='chimes-row'>
<span class='card-sub'>Browser test chimes</span>
<div class='ctrl-group'>
<button class='chip-btn' onclick='simMiiGreeting()'>Hello</button>
<button class='chip-btn' onclick='simShutter()'>Shutter</button>
<button class='chip-btn' onclick='simMiiTheme()'>Theme</button>
</div></div></div>
)rawliteral";
