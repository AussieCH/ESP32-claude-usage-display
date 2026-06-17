#pragma once
#include <stdint.h>
#include <stdbool.h>

struct Settings {
    char     sessionKey[512]; // Claude.ai session cookie value (from browser DevTools)
    char     orgId[64];       // Claude org UUID — auto-discovered, cached here
    char     wifiSsid[64];    // home WiFi SSID (needed for internet access)
    char     wifiPassword[64];
    char     apPassword[33];  // this device's AP password (min 8 chars)
    uint32_t refreshMs;
    bool     showUsagePct;    // show primary utilization % (5h or 7d)
    bool     showProgressBar;
    bool     show7dPct;       // show 7-day utilization row
    bool     show7dOpus;      // show 7-day Opus utilization row
    bool     showResetTime;   // show next reset time
    uint8_t  schemaVersion;
};

void      settingsInit();
Settings& settingsGet();
void      settingsSave(const Settings& s);
void      settingsReset();
