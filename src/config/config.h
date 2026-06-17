#pragma once

// I2C pins — GPIO22 does not exist on ESP32-S3; use GPIO8/9
#define PIN_SDA  8
#define PIN_SCL  9

#define OLED_ADDR    0x3C
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

#define AP_SSID_DEFAULT  "ESP32-Claude-Dashboard"
#define AP_PASS_DEFAULT  "dashboard1"

#define CLAUDE_ORG_URL   "https://claude.ai/api/organizations"
#define CLAUDE_USAGE_FMT "https://claude.ai/api/organizations/%s/usage"

#define REFRESH_MS_DEFAULT  30000UL

#define PREFS_NS   "claude-dash"
#define PREFS_VER  2
