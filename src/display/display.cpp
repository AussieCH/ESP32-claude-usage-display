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

// ── Display ──────────────────────────────────────────────────────────

bool displayInit() {
    Wire.begin(PIN_SDA, PIN_SCL);
    g_ready = g_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    if (!g_ready) return false;
    g_oled.clearDisplay();
    g_oled.display();
    return true;
}

void displayShowStatus(const char* msg) {
    if (!g_ready) return;
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 28);
    g_oled.print(msg);
    g_oled.display();
}

void displayShowError(const char* msg) {
    if (!g_ready) return;
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
    g_oled.clearDisplay();
    g_oled.setTextColor(SSD1306_WHITE);

    int y = 0;

    // Header
    g_oled.setTextSize(1);
    g_oled.setCursor(0, y);
    g_oled.print("CLAUDE USAGE");
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
