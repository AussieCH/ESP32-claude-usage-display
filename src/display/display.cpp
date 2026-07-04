#include "display.h"
#include "../config/config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

static Adafruit_SSD1306 g_oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool g_ready = false;

// Cached render inputs so the wink animation can redraw between fetches
static UsageData g_lastData;
static Settings  g_lastSettings;
static bool      g_hasData = false;
static uint8_t   g_animState = 0;

static void renderFrame();

// ── Time helpers ─────────────────────────────────────────────────────

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

// ── Corner icon (32x18, cropped from OLED Pixel Studio export) ──────
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

// Nearest-neighbor scaled draw (drawBitmap can't scale).
static void drawIconScaled(int x0, int y0, int w, int h) {
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * ICON_H / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * ICON_W / w;
            if (iconPixel(sx, sy))
                g_oled.drawPixel(x0 + dx, y0 + dy, SSD1306_WHITE);
        }
    }
}

// ── Display ──────────────────────────────────────────────────────────

bool displayInit() {
    Wire.begin(PIN_SDA, PIN_SCL);
    g_ready = g_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    if (!g_ready) return false;
    g_oled.clearDisplay();
    g_oled.display();
    return true;
}

const uint8_t* displayGetBuffer() {
    return g_oled.getBuffer();
}

void displayShowStatus(const char* msg) {
    if (!g_ready) return;
    g_hasData = false;   // stop animation ticks overwriting this screen
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 28);
    g_oled.print(msg);
    g_oled.display();
}

void displayShowError(const char* msg) {
    if (!g_ready) return;
    g_hasData = false;   // stop animation ticks overwriting this screen
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 0);
    g_oled.print("! ERROR");
    g_oled.setCursor(0, 14);
    g_oled.print(msg);
    g_oled.display();
}

static void drawProgressBar(int x, int y, int w, int h, uint8_t pct) {
    if (pct > 100) pct = 100;
    g_oled.drawRect(x, y, w, h, SSD1306_WHITE);
    int fill = (w - 2) * pct / 100;
    if (fill > 0) g_oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
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

    g_oled.clearDisplay();
    g_oled.setTextColor(SSD1306_WHITE);

    int y = 0;

    // Header
    g_oled.setTextSize(1);
    g_oled.setCursor(0, y);
    g_oled.print("CLAUDE USAGE");
    // Corner icon at ~1.3x (42x24): clear of header/metric text (x<=72)
    // and the progress bar (y>=31).
    drawIconScaled(OLED_WIDTH - 42, 0, 42, 24);
    y += 13;

    // Primary metric: 5-hour if available, else 7-day
    if (s.showUsagePct) {
        const UsageBlock& primary = data.fiveHour.available ? data.fiveHour : data.sevenDay;
        if (primary.available) {
            g_oled.setTextSize(2);
            g_oled.setCursor(0, y);
            g_oled.print(primary.utilization);
            g_oled.print(data.fiveHour.available ? "% 5h" : "% 7d");
            g_oled.setTextSize(1);
            y += 18;
        }
    }

    if (s.showProgressBar) {
        uint8_t pct = data.fiveHour.available ? data.fiveHour.utilization
                    : (data.sevenDay.available ? data.sevenDay.utilization : 0);
        drawProgressBar(0, y, OLED_WIDTH, 8, pct);
        y += 10;
    }

    if (s.show7dPct && data.sevenDay.available) {
        g_oled.setCursor(0, y);
        g_oled.print("7d: ");
        g_oled.print(data.sevenDay.utilization);
        g_oled.print("%");
        // "cur" not "current": full word overflows 128px (21 chars max at size 1)
        if (data.model[0]) {
            g_oled.print(" - cur: ");
            g_oled.print(data.model);
        }
        y += 9;
    }

    if (s.show7dOpus && data.sevenDayOpus.available) {
        g_oled.setCursor(0, y);
        g_oled.print("Opus: ");
        g_oled.print(data.sevenDayOpus.utilization);
        g_oled.print("%");
        y += 9;
    }

    // Reset time — size 2 cell is 16 px but glyphs only ink the top 14 px,
    // so guard at -13 lets y reach 51 without visible clipping on a 64 px display.
    if (s.showResetTime && y <= OLED_HEIGHT - 13) {
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
            // "Reset in:" size 1, countdown size 2 starting right after it
            // "Reset in:" = 9 chars × 6 px = 54 px wide
            g_oled.setTextSize(1);
            g_oled.setCursor(0, y + 4);   // +4 vertically centres 8px label within 16px timer
            g_oled.print("Reset in:");
            g_oled.setTextSize(2);
            g_oled.setCursor(56, y);
            g_oled.print(rst);
            g_oled.setTextSize(1);
        }
    }

    g_oled.display();
}
