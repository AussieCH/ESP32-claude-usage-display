#pragma once
#include <stdint.h>
#include <stdbool.h>

struct Settings {
    char     sessionKey[512]; // Claude.ai session cookie value (from browser DevTools)
    char     orgId[64];       // Claude org UUID — auto-discovered, cached here
    char     wifiSsid[64];    // home WiFi SSID (needed for internet access)
    char     wifiPassword[64];
    char     apPassword[33];  // this device's AP password (min 8 chars)
    char     proxyUrl[160];   // usage proxy URL (http://... or https://...); empty = legacy claude.ai mode
    char     proxyToken[96];  // Bearer token for the proxy (optional)
    uint32_t refreshMs;
    bool     showUsagePct;    // show primary utilization % (5h or 7d)
    bool     showProgressBar;
    bool     show7dPct;       // show 7-day utilization row
    bool     show7dOpus;      // show 7-day Opus utilization row
    bool     showResetTime;   // show next reset time
    uint8_t  schemaVersion;
    // Appended below to preserve the NVS blob layout of earlier configs (no
    // PREFS_VER bump): WiFi slots 2-4. Slot 1 is wifiSsid/wifiPassword above.
    char     wifiSsidN[3][64];
    char     wifiPasswordN[3][64];
};

#define WIFI_SLOTS 4   // slot 0 = wifiSsid/wifiPassword, slots 1-3 = wifiSsidN[]

void      settingsInit();
Settings& settingsGet();
void      settingsSave(const Settings& s);
void      settingsReset();

// WiFi slot accessors (i in 0..WIFI_SLOTS-1)
const char* wifiSsidAt(const Settings& s, int i);
const char* wifiPassAt(const Settings& s, int i);
void        wifiSetSsidAt(Settings& s, int i, const char* ssid);
void        wifiSetPassAt(Settings& s, int i, const char* pass);
bool        wifiHasAnySsid(const Settings& s);
