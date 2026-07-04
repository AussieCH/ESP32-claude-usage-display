#include "display.h"
#include "../config/config.h"
#include <TFT_eSPI.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// Full-screen sprite (320x170x16bit = ~109 KB) — TFT_eSPI allocates it in
// PSRAM automatically on boards with PSRAM enabled, so it never competes
// with the TLS stack for internal RAM.
static TFT_eSPI   g_tft;
static TFT_eSprite g_spr(&g_tft);
static bool g_ready = false;

// Cached render inputs so the wink animation can redraw between fetches
static UsageData g_lastData;
static Settings  g_lastSettings;
static bool      g_hasData = false;
static uint8_t   g_animState = 0;

// Backlight PWM (Arduino core 2.x LEDC API)
static const int BL_CHANNEL = 0;

static void renderFrame();

// ── Time helpers (unchanged from OLED version) ──────────────────────

// Parse ISO 8601 UTC string and return seconds until that moment.
// Returns -1 if time is not synced or string can't be parsed.
static int32_t secondsUntil(const char* iso) {
    if (!iso || !iso[0]) return -1;
    time_t now = time(nullptr);
    if (now < 1000000000L) return -1;  // NTP not synced yet

    struct tm t = {};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) < 5) return -1;
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = 0;
    // configTime(0,0,...) sets local=UTC so mktime is correct here
    time_t resetAt = mktime(&t);
    int32_t diff = (int32_t)(resetAt - now);
    return diff > 0 ? diff : 0;
}

static void formatCountdown(int32_t secs, char* buf, size_t len) {
    if (secs <= 0)        snprintf(buf, len, "now");
    else if (secs < 60)   snprintf(buf, len, "<1m");
    else if (secs < 3600) snprintf(buf, len, "%dm",  (int)(secs / 60));
    else                  snprintf(buf, len, "%dh%02dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
}

// ── Corner icon (32x18 source bitmap, scaled up at draw time) ───────
static const unsigned char CORNER_ICON[] PROGMEM = {
  0x1F, 0xFF, 0xFF, 0xF0,
  0x10, 0x00, 0x00, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x11, 0xF8, 0x3F, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x10, 0xF0, 0x1E, 0x10,
  0x10, 0x91, 0x12, 0x10,
  0x10, 0xF1, 0x1E, 0x10,
  0x10, 0x01, 0x00, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x10, 0x7F, 0xFC, 0x10,
  0x10, 0x80, 0x02, 0x10,
  0x10, 0x7F, 0xFC, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x10, 0x03, 0x80, 0x10,
  0x10, 0x00, 0x00, 0x10,
  0x1F, 0xFF, 0xFF, 0xF0,
};
static const int ICON_W = 32, ICON_H = 18;

// Wink animation: eyes live at source rows 5-7, cols 8-11 (left) and
// 19-22 (right). A closed eye is drawn as a lid line on row 6.
static bool g_leftEyeClosed  = false;
static bool g_rightEyeClosed = false;

// Mouth by usage: 0 smile (<30%), 1 open = original bitmap (30-60%),
// 2 closed line (60-80%), 3 sad (>80%). Mouth lives at rows 11-13.
static uint8_t g_mouth = 1;

static bool iconPixel(int sx, int sy) {
    bool inLeft  = sx >= 8  && sx <= 11 && sy >= 5 && sy <= 7;
    bool inRight = sx >= 19 && sx <= 22 && sy >= 5 && sy <= 7;
    if ((inLeft && g_leftEyeClosed) || (inRight && g_rightEyeClosed))
        return sy == 6;
    if (g_mouth != 1 && sx >= 8 && sx <= 22 && sy >= 11 && sy <= 13) {
        switch (g_mouth) {
            case 0: return (sy == 12 && (sx == 9 || sx == 21)) ||
                           (sy == 13 && sx >= 10 && sx <= 20);      // corners up
            case 2: return  sy == 12 && sx >= 9 && sx <= 21;        // flat line
            case 3: return (sy == 12 && sx >= 10 && sx <= 20) ||
                           (sy == 13 && (sx == 9 || sx == 21));     // corners down
        }
    }
    return pgm_read_byte(&CORNER_ICON[sy * 4 + sx / 8]) & (0x80 >> (sx & 7));
}

// Nearest-neighbor scaled draw into the sprite. Draws each source pixel
// as a filled block instead of per-destination-pixel lookups — cheaper
// at the ~3x scale used here.
static void drawIconScaled(int x0, int y0, int w, int h, uint16_t color) {
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * ICON_H / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * ICON_W / w;
            if (iconPixel(sx, sy))
                g_spr.drawPixel(x0 + dx, y0 + dy, color);
        }
    }
}

// ── Display ──────────────────────────────────────────────────────────

bool displayInit() {
    // The LCD (and on battery: the whole 3.3V rail behind it) is only
    // powered while GPIO15 is HIGH.
    pinMode(PIN_LCD_POWER, OUTPUT);
    digitalWrite(PIN_LCD_POWER, HIGH);

    g_tft.init();
    g_tft.setRotation(1);            // landscape, USB port on the left
    g_tft.fillScreen(COL_BG);

    // Backlight PWM on top of TFT_eSPI's plain-HIGH default
    ledcSetup(BL_CHANNEL, 5000, 8);
    ledcAttachPin(PIN_LCD_BL, BL_CHANNEL);
    displaySetBrightness(100);

    if (!g_spr.createSprite(SCREEN_W, SCREEN_H)) {
        // PSRAM missing/full — fall back to direct drawing would flicker;
        // report failure instead so the serial log points at the cause.
        return false;
    }
    g_spr.setTextWrap(false);
    g_ready = true;
    return true;
}

void displaySetBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    ledcWrite(BL_CHANNEL, (uint32_t)pct * 255 / 100);
}

void displayShowStatus(const char* msg) {
    if (!g_ready) return;
    g_hasData = false;   // stop animation ticks overwriting this screen
    g_spr.fillSprite(COL_BG);
    g_spr.setTextDatum(MC_DATUM);
    g_spr.setTextColor(COL_MUTED, COL_BG);
    g_spr.drawString(msg, SCREEN_W / 2, SCREEN_H / 2, 4);
    g_spr.setTextDatum(TL_DATUM);
    g_spr.pushSprite(0, 0);
}

void displayShowError(const char* msg) {
    if (!g_ready) return;
    g_hasData = false;   // stop animation ticks overwriting this screen
    g_spr.fillSprite(COL_BG);
    g_spr.setTextColor(COL_RED, COL_BG);
    g_spr.drawString("! ERROR", 10, 40, 4);
    g_spr.setTextColor(COL_TEXT, COL_BG);
    g_spr.drawString(msg, 10, 80, 4);
    g_spr.pushSprite(0, 0);
}

static uint16_t barColor(uint8_t pct) {
    if (pct > 80) return COL_RED;
    if (pct >= 60) return COL_AMBER;
    return COL_GREEN;
}

static void drawProgressBar(int x, int y, int w, int h, uint8_t pct) {
    if (pct > 100) pct = 100;
    g_spr.drawRoundRect(x, y, w, h, 3, COL_MUTED);
    int fill = (w - 4) * pct / 100;
    if (fill > 0) g_spr.fillRoundRect(x + 2, y + 2, fill, h - 4, 2, barColor(pct));
}

void displayRender(const UsageData& data, const Settings& s) {
    if (!g_ready) return;
    g_lastData     = data;
    g_lastSettings = s;
    g_hasData      = true;
    renderFrame();
}

// Wink cycle, repeats every 7 s: left eye closes, then right, then they
// reopen in the same order, 500 ms per step.
void displayTick() {
    if (!g_ready || !g_hasData) return;
    uint32_t p = millis() % 7000;
    uint8_t st = (p < 500) ? 1 : (p < 1000) ? 2 : (p < 1500) ? 3 : 0;
    if (st == g_animState) return;
    g_animState      = st;
    g_leftEyeClosed  = (st == 1 || st == 2);
    g_rightEyeClosed = (st == 2 || st == 3);
    renderFrame();
}

static void renderFrame() {
    const UsageData& data = g_lastData;
    const Settings&  s    = g_lastSettings;

    const UsageBlock& pm = data.fiveHour.available ? data.fiveHour : data.sevenDay;
    uint8_t u = pm.available ? pm.utilization : 0;
    g_mouth = (u < 30) ? 0 : (u < 60) ? 1 : (u <= 80) ? 2 : 3;

    g_spr.fillSprite(COL_BG);

    // Header + face icon (top-right, ~3x scale of the 32x18 source)
    g_spr.setTextColor(COL_ORANGE, COL_BG);
    g_spr.drawString("CLAUDE USAGE", 10, 8, 4);
    drawIconScaled(SCREEN_W - 102, 4, 96, 54, COL_ORANGE);

    int y = 40;

    // Primary metric: 5-hour if available, else 7-day
    if (s.showUsagePct) {
        const UsageBlock& primary = data.fiveHour.available ? data.fiveHour : data.sevenDay;
        if (primary.available) {
            char big[16];
            snprintf(big, sizeof(big), "%d%%", primary.utilization);
            g_spr.setTextColor(COL_TEXT, COL_BG);
            g_spr.setTextSize(2);
            int w = g_spr.drawString(big, 10, y, 4);   // font 4 @ 2x = 52 px
            g_spr.setTextSize(1);
            g_spr.setTextColor(COL_MUTED, COL_BG);
            g_spr.drawString(data.fiveHour.available ? "5h" : "7d", 10 + w + 8, y + 24, 4);
            y += 56;
        }
    }

    if (s.showProgressBar) {
        uint8_t pct = data.fiveHour.available ? data.fiveHour.utilization
                    : (data.sevenDay.available ? data.sevenDay.utilization : 0);
        drawProgressBar(10, y, SCREEN_W - 20, 14, pct);
        y += 22;
    }

    // 7d + Opus share one row — the wide screen has the room, and it keeps
    // the reset countdown on screen even with everything enabled.
    if ((s.show7dPct && data.sevenDay.available) ||
        (s.show7dOpus && data.sevenDayOpus.available)) {
        int x = 10;
        g_spr.setTextColor(COL_MUTED, COL_BG);
        if (s.show7dPct && data.sevenDay.available) {
            char row[40];
            snprintf(row, sizeof(row), "7d: %d%%", data.sevenDay.utilization);
            x += g_spr.drawString(row, x, y, 4);
            if (data.model[0]) {
                char cur[36];
                snprintf(cur, sizeof(cur), "  cur: %s", data.model);
                x += g_spr.drawString(cur, x, y, 4);
            }
        }
        if (s.show7dOpus && data.sevenDayOpus.available) {
            char row[32];
            snprintf(row, sizeof(row), "  Opus: %d%%", data.sevenDayOpus.utilization);
            g_spr.drawString(row, x, y, 4);
        }
        y += 30;
    }

    // Reset countdown — font 4 is 26 px high, keep it fully on screen
    if (s.showResetTime && y <= SCREEN_H - 28) {
        const UsageBlock& ref = data.fiveHour.available ? data.fiveHour : data.sevenDay;
        if (ref.resetsAt[0]) {
            char rst[10];
            int32_t secs = secondsUntil(ref.resetsAt);
            if (secs >= 0) {
                formatCountdown(secs, rst, sizeof(rst));
            } else {
                const char* t = strchr(ref.resetsAt, 'T');
                if (t && strlen(t) >= 6) snprintf(rst, sizeof(rst), "%.5s", t + 1);
                else snprintf(rst, sizeof(rst), "?");
            }
            g_spr.setTextColor(COL_MUTED, COL_BG);
            int w = g_spr.drawString("Reset in: ", 10, y, 4);
            g_spr.setTextColor(COL_ORANGE, COL_BG);
            g_spr.drawString(rst, 10 + w, y, 4);
        }
    }

    g_spr.pushSprite(0, 0);
}
