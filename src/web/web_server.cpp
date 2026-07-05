#include "web_server.h"
#include "../storage/settings.h"
#include "../models/usage_data.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <string.h>

static AsyncWebServer g_server(80);
static UsageData*     g_data = nullptr;

static char   s_postBuf[2200];   // headroom for proxyUrl+proxyToken + 4 WiFi slots
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
canvas{border:2px solid #44403c;border-radius:6px;display:block;margin:0 auto;image-rendering:pixelated;max-width:100%;height:auto}
.meta{font-size:.75rem;color:#78716c;margin-top:6px;text-align:center}
</style>
</head>
<body>
<h1>Claude Usage Dashboard</h1>

<div class="card">
  <h2>Live Preview</h2>
  <div id="oled-wrap">
    <canvas id="oled" width="640" height="340"></canvas>
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
    <span>WiFi Networks (up to 4)</span>
    <div class="hint">The device connects to whichever configured network is in range — add home, office, etc. It auto-switches when you move.</div>
  </div>
  <div id="wifiList"></div>

  <div class="field">
    <span>Usage Proxy URL (recommended)</span>
    <input type="text" id="proxyUrl" placeholder="http://192.168.x.x:8787/usage or https://node.tailnet.ts.net/usage">
    <div class="hint">If set, the device polls your local/Tailscale proxy instead of claude.ai — no cookie needed. See proxy/README.md.</div>
  </div>

  <div class="field">
    <span>Proxy Bearer Token</span>
    <input type="password" id="proxyToken" placeholder="Leave blank to keep current / none">
  </div>

  <div class="field">
    <span>Claude Cookie Header (legacy mode)</span>
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
const S = 2, W = 320, H = 170;
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
    const wifi = cfg.wifi || [];
    const list = document.getElementById('wifiList');
    list.innerHTML = '';
    for (let i = 0; i < 4; i++) {
      list.insertAdjacentHTML('beforeend',
        '<div class="field"><span>WiFi ' + (i+1) + '</span>' +
        '<input type="text" id="wifiSsid' + i + '" placeholder="Network name (blank = unused)">' +
        '<input type="password" id="wifiPass' + i + '" placeholder="Password (blank = keep current)" style="margin-top:6px">' +
        '</div>');
      document.getElementById('wifiSsid' + i).value = (wifi[i] && wifi[i].ssid) || '';
    }
    document.getElementById('proxyUrl').value    = cfg.proxyUrl   || '';
    document.getElementById('proxyToken').value  = '';
    document.getElementById('refreshMs').value   = cfg.refreshMs  || 30000;
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
    proxyUrl:        document.getElementById('proxyUrl').value.trim(),
    refreshMs:       parseInt(document.getElementById('refreshMs').value) || 30000,
  };
  body.wifi = [];
  for (let i = 0; i < 4; i++) {
    const slot = { ssid: document.getElementById('wifiSsid' + i).value.trim() };
    const pw = document.getElementById('wifiPass' + i).value;
    if (pw) slot.password = pw;
    body.wifi.push(slot);
  }
  const sk = document.getElementById('sessionKey').value.trim();
  const ap = document.getElementById('apPassword').value;
  const pt = document.getElementById('proxyToken').value.trim();
  if (sk) body.sessionKey   = sk;
  if (ap) body.apPassword   = ap;
  if (pt) body.proxyToken   = pt;

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
  const ORANGE = '#d97757', MUTED = '#a8a29e', TEXT = '#fff';
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W * S, H * S);

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

  // Header + face icon (mirrors drawIconScaled(W-102, 4, 96, 54) in orange)
  ctx.fillStyle = ORANGE;
  ctx.font = 'bold ' + (20 * S) + 'px monospace';
  ctx.fillText('CLAUDE USAGE', 10 * S, 28 * S);

  const ICON = [0x1FFFFFF0,0x10000010,0x10000010,0x11F83F10,0x10000010,
                0x10F01E10,0x10911210,0x10F11E10,0x10010010,0x10000010,
                0x10000010,0x107FFC10,0x10800210,0x107FFC10,0x10000010,
                0x10038010,0x10000010,0x1FFFFFF0];
  for (let dy = 0; dy < 54; dy++) {
    const sy = Math.floor(dy * 18 / 54);
    for (let dx = 0; dx < 96; dx++) {
      const sx = Math.floor(dx * 32 / 96);
      if ((ICON[sy] >>> (31 - sx)) & 1) ctx.fillRect((W - 102 + dx) * S, (4 + dy) * S, S, S);
    }
  }

  let y = 40;

  if (c.showUsagePct) {
    const primary = fh.available ? fh : sd;
    if (primary.available) {
      const pct = (primary.utilization || 0) + '%';
      ctx.fillStyle = TEXT;
      ctx.font = 'bold ' + (42 * S) + 'px monospace';
      ctx.fillText(pct, 10 * S, (y + 42) * S);
      const w = ctx.measureText(pct).width;
      ctx.fillStyle = MUTED;
      ctx.font = (20 * S) + 'px monospace';
      ctx.fillText(fh.available ? '5h' : '7d', 10 * S + w + 8 * S, (y + 42) * S);
      y += 56;
    }
  }

  if (c.showProgressBar) {
    const pct = fh.available ? (fh.utilization || 0) : (sd.available ? (sd.utilization || 0) : 0);
    ctx.strokeStyle = MUTED;
    ctx.lineWidth = S;
    ctx.strokeRect(10 * S, y * S, (W - 20) * S, 14 * S);
    ctx.fillStyle = pct > 80 ? '#f87171' : (pct >= 60 ? '#fbbf24' : '#34d399');
    const fill = Math.round((W - 24) * Math.min(pct, 100) / 100);
    if (fill > 0) ctx.fillRect(12 * S, (y + 2) * S, fill * S, 10 * S);
    y += 22;
  }

  ctx.font = (18 * S) + 'px monospace';
  ctx.fillStyle = MUTED;

  if ((c.show7dPct && sd.available) || (c.show7dOpus && so.available)) {
    let row = '';
    if (c.show7dPct && sd.available) {
      row += '7d: ' + (sd.utilization || 0) + '%';
      if (live.model) row += '  cur: ' + live.model;
    }
    if (c.show7dOpus && so.available) row += '  Opus: ' + (so.utilization || 0) + '%';
    ctx.fillText(row.trim(), 10 * S, (y + 18) * S);
    y += 30;
  }

  if (c.showResetTime && y <= H - 28) {
    const ref = fh.available ? fh : sd;
    if (ref.resetsAt) {
      try {
        const dt = new Date(ref.resetsAt);
        const label = 'Reset in: ';
        ctx.fillStyle = MUTED;
        ctx.fillText(label, 10 * S, (y + 18) * S);
        ctx.fillStyle = ORANGE;
        ctx.fillText(dt.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}),
                     10 * S + ctx.measureText(label).width, (y + 18) * S);
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
    doc["proxyUrl"]        = s.proxyUrl;
    doc["proxyToken"]      = "";            // never echoed
    JsonArray wifi = doc["wifi"].to<JsonArray>();
    for (int i = 0; i < WIFI_SLOTS; i++) {
        JsonObject o = wifi.add<JsonObject>();
        o["ssid"] = wifiSsidAt(s, i);       // passwords never echoed
    }
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
    doc["model"]     = g_data->model;

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
    if (!doc["proxyUrl"].isNull())
        snprintf(s.proxyUrl, sizeof(s.proxyUrl), "%s", doc["proxyUrl"].as<const char*>());
    if (!doc["proxyToken"].isNull()) {
        const char* pt = doc["proxyToken"].as<const char*>();
        if (pt && strlen(pt) > 0)
            snprintf(s.proxyToken, sizeof(s.proxyToken), "%s", pt);
    }
    if (doc["wifi"].is<JsonArray>()) {
        JsonArray wifi = doc["wifi"].as<JsonArray>();
        for (int i = 0; i < WIFI_SLOTS && i < (int)wifi.size(); i++) {
            JsonObject o = wifi[i].as<JsonObject>();
            if (o.isNull()) continue;
            if (!o["ssid"].isNull())                    // SSID set even if empty (clears slot)
                wifiSetSsidAt(s, i, o["ssid"].as<const char*>());
            const char* pw = o["password"].as<const char*>();
            if (pw && strlen(pw) > 0)                   // password only if provided
                wifiSetPassAt(s, i, pw);
        }
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
