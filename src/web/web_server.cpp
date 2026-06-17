#include "web_server.h"
#include "../storage/settings.h"
#include "../models/usage_data.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <string.h>

static AsyncWebServer g_server(80);
static UsageData*     g_data = nullptr;

static char   s_postBuf[1200];
static size_t s_postBufLen = 0;

static const char HTML_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Dashboard</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#1c1917;color:#e7e5e4;padding:16px;min-height:100vh}
h1{color:#d97757;font-size:1.3rem;margin-bottom:16px;letter-spacing:.03em}
h2{font-size:.78rem;text-transform:uppercase;letter-spacing:.12em;color:#a8a29e;margin-bottom:10px}
.card{background:#292524;border:1px solid #44403c;border-radius:10px;padding:16px;margin-bottom:14px}
label.toggle{display:flex;align-items:center;gap:10px;margin:6px 0;font-size:.9rem;cursor:pointer}
label.toggle input{width:16px;height:16px;accent-color:#d97757;cursor:pointer}
.field{margin:10px 0}
.field>span{display:block;font-size:.8rem;color:#a8a29e;margin-bottom:4px}
.field .hint{font-size:.72rem;color:#78716c;margin-top:3px}
input[type=text],input[type=password],input[type=number]{
  width:100%;background:#1c1917;color:#e7e5e4;border:1px solid #57534e;
  border-radius:6px;padding:7px 10px;font-size:.9rem}
input:focus{outline:none;border-color:#d97757}
.readonly{opacity:.45;pointer-events:none}
.btn-row{display:flex;gap:10px;margin-top:14px;flex-wrap:wrap}
button{border:none;border-radius:6px;padding:8px 18px;cursor:pointer;font-size:.88rem;font-weight:600;transition:background .15s}
.btn-save{background:#d97757;color:#fff}.btn-save:hover{background:#c2603a}
.btn-reset{background:#44403c;color:#d6d3d1}.btn-reset:hover{background:#57534e}
.btn-refresh{background:#1a3d2b;color:#86efac}.btn-refresh:hover{background:#166534}
#status{font-size:.8rem;margin-top:8px;min-height:1.2em;transition:color .2s}
#oled-wrap{text-align:center;padding:8px 0}
canvas{border:2px solid #44403c;border-radius:6px;display:block;margin:0 auto;image-rendering:pixelated}
.meta{font-size:.75rem;color:#78716c;margin-top:6px;text-align:center}
</style>
</head>
<body>
<h1>Claude Usage Dashboard</h1>

<div class="card">
  <h2>Live Preview</h2>
  <div id="oled-wrap">
    <canvas id="oled" width="384" height="192"></canvas>
    <div class="meta" id="ts">No data yet</div>
  </div>
</div>

<div class="card">
  <h2>Display Settings</h2>
  <label class="toggle"><input type="checkbox" id="showUsagePct"> Primary % (5h or 7d)</label>
  <label class="toggle"><input type="checkbox" id="showProgressBar"> Progress Bar</label>
  <label class="toggle"><input type="checkbox" id="show7dPct"> 7-day %</label>
  <label class="toggle"><input type="checkbox" id="show7dOpus"> 7-day Opus %</label>
  <label class="toggle"><input type="checkbox" id="showResetTime"> Reset Time</label>
</div>

<div class="card">
  <h2>Connection Settings</h2>

  <div class="field">
    <span>Home WiFi SSID</span>
    <input type="text" id="wifiSsid" placeholder="Your home network name">
    <div class="hint">Device must connect here to reach claude.ai</div>
  </div>
  <div class="field">
    <span>Home WiFi Password</span>
    <input type="password" id="wifiPassword" placeholder="Leave blank to keep current">
  </div>

  <div class="field">
    <span>Claude Cookie Header</span>
    <input type="password" id="sessionKey" placeholder="Paste full Cookie header value">
    <div class="hint">Chrome DevTools → Network → any claude.ai request → Request Headers → <b>Cookie</b> → copy full value</div>
  </div>

  <div class="field">
    <span>Org ID (auto-discovered)</span>
    <input type="text" id="orgIdDisplay" class="readonly" readonly>
  </div>

  <div class="field">
    <span>Refresh Interval (ms)</span>
    <input type="number" id="refreshMs" min="5000" step="1000">
  </div>

  <div class="field">
    <span>AP Password (min 8 chars)</span>
    <input type="password" id="apPassword" placeholder="Leave blank to keep current">
  </div>

  <div class="btn-row">
    <button class="btn-save" onclick="saveSettings()">Save Settings</button>
    <button class="btn-refresh" onclick="refreshNow()">Refresh Data</button>
    <button class="btn-reset" onclick="resetSettings()">Reset Defaults</button>
  </div>
  <div id="status"></div>
</div>

<script>
const S = 3, W = 128, H = 64;
let cfg = {}, live = {};

function setStatus(msg, ok) {
  const el = document.getElementById('status');
  el.style.color = ok === false ? '#f87171' : '#34d399';
  el.textContent = msg;
  if (ok !== false) setTimeout(() => { el.textContent = ''; }, 3000);
}

async function loadSettings() {
  try {
    const r = await fetch('/api/settings');
    cfg = await r.json();
    document.getElementById('showUsagePct').checked   = !!cfg.showUsagePct;
    document.getElementById('showProgressBar').checked = !!cfg.showProgressBar;
    document.getElementById('show7dPct').checked      = !!cfg.show7dPct;
    document.getElementById('show7dOpus').checked     = !!cfg.show7dOpus;
    document.getElementById('showResetTime').checked  = !!cfg.showResetTime;
    document.getElementById('wifiSsid').value    = cfg.wifiSsid   || '';
    document.getElementById('refreshMs').value   = cfg.refreshMs  || 30000;
    document.getElementById('wifiPassword').value = '';
    document.getElementById('apPassword').value  = '';
    document.getElementById('sessionKey').value  = '';
    document.getElementById('orgIdDisplay').value = cfg.orgId || '(not yet discovered)';
  } catch(e) { setStatus('Failed to load settings', false); }
}

async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    live = await r.json();
    const fh = (live.fiveHour || {});
    let label = live.valid
      ? (fh.available ? '5h: ' + (fh.utilization||0) + '%' : '') +
        ' — ' + (live.timestamp || '')
      : 'Waiting for data…';
    document.getElementById('ts').textContent = label.trim();
    drawPreview();
  } catch(e) {}
}

async function saveSettings() {
  const body = {
    showUsagePct:    document.getElementById('showUsagePct').checked,
    showProgressBar: document.getElementById('showProgressBar').checked,
    show7dPct:       document.getElementById('show7dPct').checked,
    show7dOpus:      document.getElementById('show7dOpus').checked,
    showResetTime:   document.getElementById('showResetTime').checked,
    wifiSsid:        document.getElementById('wifiSsid').value.trim(),
    refreshMs:       parseInt(document.getElementById('refreshMs').value) || 30000,
  };
  const sk = document.getElementById('sessionKey').value.trim();
  const wp = document.getElementById('wifiPassword').value;
  const ap = document.getElementById('apPassword').value;
  if (sk) body.sessionKey   = sk;
  if (wp) body.wifiPassword = wp;
  if (ap) body.apPassword   = ap;

  try {
    const r = await fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (r.ok) {
      cfg = Object.assign(cfg, body);
      setStatus('Saved! Device will use new settings on next fetch.');
      drawPreview();
    } else setStatus('Save failed', false);
  } catch(e) { setStatus('Save failed', false); }
}

async function resetSettings() {
  if (!confirm('Reset all settings to defaults?')) return;
  try {
    const r = await fetch('/api/reset', { method: 'POST' });
    if (r.ok) { setStatus('Reset to defaults'); await loadSettings(); }
    else setStatus('Reset failed', false);
  } catch(e) { setStatus('Reset failed', false); }
}

async function refreshNow() {
  try {
    await fetch('/api/refresh', { method: 'POST' });
    setTimeout(loadStatus, 2000);
  } catch(e) {}
}

function drawPreview() {
  const canvas = document.getElementById('oled');
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W * S, H * S);
  ctx.fillStyle = '#fff';

  const fh = (live.fiveHour || {});
  const sd = (live.sevenDay || {});
  const so = (live.sevenDayOpus || {});
  const c = {
    showUsagePct:    document.getElementById('showUsagePct').checked,
    showProgressBar: document.getElementById('showProgressBar').checked,
    show7dPct:       document.getElementById('show7dPct').checked,
    show7dOpus:      document.getElementById('show7dOpus').checked,
    showResetTime:   document.getElementById('showResetTime').checked,
  };

  let y = 0;

  ctx.font = (7 * S) + 'px monospace';
  ctx.fillText('CLAUDE USAGE', 0, (y + 7) * S);
  y += 10;

  if (c.showUsagePct) {
    const primary = fh.available ? fh : sd;
    if (primary.available) {
      ctx.font = 'bold ' + (14 * S) + 'px monospace';
      ctx.fillText((primary.utilization || 0) + (fh.available ? '% 5h' : '% 7d'), 0, (y + 14) * S);
      ctx.font = (7 * S) + 'px monospace';
      y += 18;
    }
  }

  if (c.showProgressBar) {
    const pct = fh.available ? (fh.utilization || 0) : (sd.available ? (sd.utilization || 0) : 0);
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 1;
    ctx.strokeRect(0.5 * S, (y + 0.5) * S, (W - 1) * S, 7 * S);
    const fill = Math.round((W - 2) * Math.min(pct, 100) / 100);
    if (fill > 0) ctx.fillRect(1 * S, (y + 1) * S, fill * S, 5 * S);
    y += 11;
  }

  ctx.font = (7 * S) + 'px monospace';

  if (c.show7dPct && sd.available) {
    ctx.fillText('7d: ' + (sd.utilization || 0) + '%', 0, (y + 7) * S);
    y += 9;
  }

  if (c.show7dOpus && so.available) {
    ctx.fillText('Opus: ' + (so.utilization || 0) + '%', 0, (y + 7) * S);
    y += 9;
  }

  if (c.showResetTime && y < H - 8) {
    const ref = fh.available ? fh : sd;
    if (ref.resetsAt) {
      try {
        const dt = new Date(ref.resetsAt);
        ctx.fillText('Rst:' + dt.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}),
                     0, (y + 7) * S);
      } catch(e) {}
    }
  }
}

document.querySelectorAll('input[type=checkbox]').forEach(el => {
  el.addEventListener('change', drawPreview);
});

loadSettings().then(drawPreview);
loadStatus();
setInterval(loadStatus, 30000);
</script>
</body>
</html>
)rawhtml";

// ── Route handlers ──────────────────────────────────────────────────

static void handleRoot(AsyncWebServerRequest* req) {
    req->send(200, "text/html", HTML_PAGE);
}

static void handleGetSettings(AsyncWebServerRequest* req) {
    const Settings& s = settingsGet();
    JsonDocument doc;
    doc["sessionKey"]      = "";            // never echoed
    doc["orgId"]           = s.orgId;
    doc["wifiSsid"]        = s.wifiSsid;
    doc["wifiPassword"]    = "";            // never echoed
    doc["apPassword"]      = "";            // never echoed
    doc["refreshMs"]       = s.refreshMs;
    doc["showUsagePct"]    = s.showUsagePct;
    doc["showProgressBar"] = s.showProgressBar;
    doc["show7dPct"]       = s.show7dPct;
    doc["show7dOpus"]      = s.show7dOpus;
    doc["showResetTime"]   = s.showResetTime;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetStatus(AsyncWebServerRequest* req) {
    if (!g_data) { req->send(503); return; }

    JsonDocument doc;
    doc["valid"]     = g_data->valid;
    doc["timestamp"] = g_data->timestamp;

    auto addBlock = [&](const char* key, const UsageBlock& b) {
        JsonObject o = doc[key].to<JsonObject>();
        o["utilization"] = b.utilization;
        o["resetsAt"]    = b.resetsAt;
        o["available"]   = b.available;
    };
    addBlock("fiveHour",    g_data->fiveHour);
    addBlock("sevenDay",    g_data->sevenDay);
    addBlock("sevenDayOpus", g_data->sevenDayOpus);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void applySettingsJson(const char* buf, size_t len, AsyncWebServerRequest* req) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) {
        req->send(400, "application/json", "{\"error\":\"parse error\"}");
        return;
    }

    Settings s = settingsGet();

    if (!doc["sessionKey"].isNull()) {
        const char* sk = doc["sessionKey"].as<const char*>();
        if (sk && strlen(sk) > 0) {
            if (strcmp(s.sessionKey, sk) != 0)
                s.orgId[0] = '\0';  // new key → force org rediscovery
            snprintf(s.sessionKey, sizeof(s.sessionKey), "%s", sk);
        }
    }
    if (!doc["wifiSsid"].isNull())
        snprintf(s.wifiSsid, sizeof(s.wifiSsid), "%s", doc["wifiSsid"].as<const char*>());
    if (!doc["wifiPassword"].isNull()) {
        const char* pw = doc["wifiPassword"].as<const char*>();
        if (pw && strlen(pw) > 0)
            snprintf(s.wifiPassword, sizeof(s.wifiPassword), "%s", pw);
    }
    if (!doc["apPassword"].isNull()) {
        const char* pw = doc["apPassword"].as<const char*>();
        if (pw && strlen(pw) >= 8)
            snprintf(s.apPassword, sizeof(s.apPassword), "%s", pw);
    }
    if (!doc["refreshMs"].isNull())
        s.refreshMs = max((uint32_t)5000, doc["refreshMs"].as<uint32_t>());
    if (!doc["showUsagePct"].isNull())    s.showUsagePct    = doc["showUsagePct"].as<bool>();
    if (!doc["showProgressBar"].isNull()) s.showProgressBar = doc["showProgressBar"].as<bool>();
    if (!doc["show7dPct"].isNull())       s.show7dPct       = doc["show7dPct"].as<bool>();
    if (!doc["show7dOpus"].isNull())      s.show7dOpus      = doc["show7dOpus"].as<bool>();
    if (!doc["showResetTime"].isNull())   s.showResetTime   = doc["showResetTime"].as<bool>();

    settingsSave(s);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handlePostSettingsBody(AsyncWebServerRequest* req, uint8_t* data,
                                   size_t len, size_t index, size_t total) {
    if (index == 0) s_postBufLen = 0;
    size_t room = sizeof(s_postBuf) - s_postBufLen - 1;
    size_t copy = len < room ? len : room;
    memcpy(s_postBuf + s_postBufLen, data, copy);
    s_postBufLen += copy;
    if (index + len >= total) {
        s_postBuf[s_postBufLen] = '\0';
        applySettingsJson(s_postBuf, s_postBufLen, req);
    }
}

static void handleReset(AsyncWebServerRequest* req) {
    settingsReset();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleRefresh(AsyncWebServerRequest* req) {
    extern volatile bool g_forceRefresh;
    g_forceRefresh = true;
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── Public ──────────────────────────────────────────────────────────

void webServerStart(UsageData& usageData) {
    g_data = &usageData;

    g_server.on("/",             HTTP_GET, handleRoot);
    g_server.on("/api/settings", HTTP_GET, handleGetSettings);
    g_server.on("/api/status",   HTTP_GET, handleGetStatus);

    g_server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        handlePostSettingsBody);

    g_server.on("/api/reset", HTTP_POST,
        [](AsyncWebServerRequest* req) { handleReset(req); });

    g_server.on("/api/refresh", HTTP_POST,
        [](AsyncWebServerRequest* req) { handleRefresh(req); });

    g_server.onNotFound([](AsyncWebServerRequest* req) { req->send(404); });
    g_server.begin();
    Serial.println("[Web] Server started on port 80");
}
