#pragma once

// ── LILYGO T-Display S3 board pins ───────────────────────────────────
// LCD pins (ST7789, 8-bit parallel) are defined via build_flags in
// platformio.ini for TFT_eSPI. Only board-level pins live here.

#define PIN_LCD_POWER  15   // must be HIGH or the LCD stays dark on battery
#define PIN_LCD_BL     38   // backlight, PWM-dimmable (LEDC)

#define PIN_BTN_BOOT   0    // left button:  force data refresh
#define PIN_BTN_KEY    14   // right button: cycle backlight brightness

// Landscape orientation: 320 wide x 170 high
#define SCREEN_W  320
#define SCREEN_H  170

// ── Colors (RGB565) ──────────────────────────────────────────────────
#define COL_BG      0x0000  // black
#define COL_TEXT    0xFFFF  // white
#define COL_MUTED   0xAD13  // #a8a29e  stone grey
#define COL_ORANGE  0xDBAA  // #d97757  Claude orange
#define COL_GREEN   0x3693  // #34d399  bar < 60%
#define COL_AMBER   0xFDE4  // #fbbf24  bar 60-80%
#define COL_RED     0xFB8E  // #f87171  bar > 80%

#define AP_SSID_DEFAULT  "ESP32-Claude-Dashboard"
#define AP_PASS_DEFAULT  "dashboard1"

#define CLAUDE_ORG_URL   "https://claude.ai/api/organizations"
#define CLAUDE_USAGE_FMT "https://claude.ai/api/organizations/%s/usage"

#define REFRESH_MS_DEFAULT  30000UL

#define PREFS_NS   "claude-dash"
#define PREFS_VER  3   // v3: added proxyUrl/proxyToken
