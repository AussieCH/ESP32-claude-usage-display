#include "display.h"
#include "../config/config.h"
#include <TFT_eSPI.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
// FreeSansBold GFX fonts are bundled by TFT_eSPI.h (LOAD_GFXFF) — no include needed

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

// ── Time helpers ────────────────────────────────────────────────────
// The device clock is UTC (NTP); TZ is set to Europe/Zurich in main.cpp,
// so localtime_r() yields local time with DST for absolute display.

// UTC broken-down time -> epoch (portable timegm; independent of TZ).
static time_t utcMktime(const struct tm* t) {
    int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}

// Parse ISO 8601 UTC string to epoch. Returns 0 on failure.
static time_t parseIsoUtc(const char* iso) {
    if (!iso || !iso[0]) return 0;
    struct tm t = {};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) < 5) return 0;
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    return utcMktime(&t);
}

// Seconds until an ISO UTC moment. -1 if clock not synced / unparsable.
static int32_t secondsUntil(const char* iso) {
    time_t now = time(nullptr);
    if (now < 1000000000L) return -1;  // NTP not synced yet
    time_t at = parseIsoUtc(iso);
    if (!at) return -1;
    int32_t diff = (int32_t)(at - now);
    return diff > 0 ? diff : 0;
}

// "H:MM" countdown (e.g. "2:15"); "--:--" if not synced.
static void formatHHMM(int32_t secs, char* buf, size_t len) {
    if (secs < 0) { snprintf(buf, len, "--:--"); return; }
    snprintf(buf, len, "%d:%02d", (int)(secs / 3600), (int)((secs % 3600) / 60));
}

// Absolute local date+time of an ISO UTC moment as "DD.MM HH:MM"
// (local zone = whatever TZ main.cpp set). "--" if not synced/unparsable.
static void formatLocalDT(const char* iso, char* buf, size_t len) {
    time_t at = parseIsoUtc(iso);
    if (!at || time(nullptr) < 1000000000L) { snprintf(buf, len, "--"); return; }
    struct tm lt;
    localtime_r(&at, &lt);
    strftime(buf, len, "%d.%m. %H:%M", &lt);
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

// Center-percentage colour by usage band (per spec):
// 0-30 green, 31-60 yellow, 61-80 orange, 81-100 red.
static uint16_t gaugeTextColor(uint8_t pct) {
    if (pct <= 30) return COL_GREEN;
    if (pct <= 60) return COL_AMBER;    // yellow
    if (pct <= 80) return COL_ORANGE2;  // orange
    return COL_RED;
}

// Draw the gauge ring (annulus) gap-free by testing every pixel in the
// bounding box — no radial seams. Angle 0 = top (12 o'clock), clockwise.
// Pixels within the swept angle get `fill`, the rest get `track`.
static void fillRingArc(int cx, int cy, int rO, int rI, float sweepDeg, uint16_t track, uint16_t fill) {
    const int rO2 = rO * rO, rI2 = rI * rI;
    for (int dy = -rO; dy <= rO; dy++) {
        for (int dx = -rO; dx <= rO; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > rO2 || d2 < rI2) continue;
            float ang = atan2f((float)dx, (float)-dy) * 57.2957795f;  // 0=top, CW
            if (ang < 0) ang += 360.0f;
            g_spr.drawPixel(cx + dx, cy + dy, ang <= sweepDeg ? fill : track);
        }
    }
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

    // Primary metric drives the ring + the face: 5-hour if available, else 7-day
    const bool has5h = data.fiveHour.available;
    const UsageBlock& pm = has5h ? data.fiveHour : data.sevenDay;
    const uint8_t u = pm.available ? pm.utilization : 0;
    g_mouth = (u < 30) ? 0 : (u < 60) ? 1 : (u <= 80) ? 2 : 3;

    g_spr.fillSprite(COL_BG);

    // Header (top-left) + face icon (top-right)
    g_spr.setTextDatum(TL_DATUM);
    g_spr.setTextColor(COL_ORANGE, COL_BG);
    g_spr.drawString("CLAUDE USAGE", 10, 6, 4);
    drawIconScaled(238, 6, 68, 38, COL_ORANGE);

    // 5-hour reset countdown (H:MM) directly under the face icon
    if (s.showResetTime && has5h && data.fiveHour.resetsAt[0]) {
        char cd[8];
        formatHHMM(secondsUntil(data.fiveHour.resetsAt), cd, sizeof(cd));
        g_spr.setTextDatum(MC_DATUM);
        g_spr.setTextColor(COL_MUTED, COL_BG);
        g_spr.drawString("5h reset", 272, 52, 2);
        g_spr.setTextColor(COL_TEXT, COL_BG);
        g_spr.drawString(cd, 272, 74, 4);
    }

    // Ring gauge (blue) with the percentage in the centre (colour by band)
    if (s.showUsagePct) {
        const int CX = 78, CY = 102, RO = 60, RI = 47;
        fillRingArc(CX, CY, RO, RI, pm.available ? u * 3.6f : 0.0f, COL_TRACK, COL_BLUE);

        // small "5h"/"7d" label (GLCD font) first, before switching fonts
        g_spr.setTextDatum(MC_DATUM);
        g_spr.setTextColor(COL_MUTED, COL_BG);
        g_spr.drawString(has5h ? "5h" : "7d", CX, CY + 30, 2);

        // centre percentage in a smooth GFX font, colour by band, auto-fit
        char pct[8];
        if (pm.available) snprintf(pct, sizeof(pct), "%d%%", u);
        else              snprintf(pct, sizeof(pct), "--");
        g_spr.setTextColor(pm.available ? gaugeTextColor(u) : COL_MUTED, COL_BG);
        g_spr.setFreeFont(&FreeSansBold24pt7b);
        if (g_spr.textWidth(pct) > 2 * RI - 4) g_spr.setFreeFont(&FreeSansBold18pt7b);
        g_spr.drawString(pct, CX, CY - 6);
        g_spr.setTextFont(2);   // clear gfxFont so the GLCD-font draws below work
    }

    // 7-day: percentage + absolute local date/time of the next 7d reset
    if (s.show7dPct && data.sevenDay.available) {
        g_spr.setTextDatum(TL_DATUM);
        char row[24];
        snprintf(row, sizeof(row), "7 Days: %d%%", data.sevenDay.utilization);
        g_spr.setTextColor(COL_TEXT, COL_BG);
        g_spr.drawString(row, 160, 100, 4);

        if (data.sevenDay.resetsAt[0]) {
            char dt[24], line[36];
            formatLocalDT(data.sevenDay.resetsAt, dt, sizeof(dt));
            snprintf(line, sizeof(line), "Reset %s", dt);
            g_spr.setTextColor(COL_MUTED, COL_BG);
            g_spr.drawString(line, 160, 134, 2);
        }
    }

    g_spr.setTextDatum(TL_DATUM);
    g_spr.pushSprite(0, 0);
}
