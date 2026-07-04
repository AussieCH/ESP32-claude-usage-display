#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config/config.h"
#include "models/usage_data.h"
#include "storage/settings.h"
#include "display/display.h"
#include "network/wifi_manager.h"
#include "services/api_service.h"
#include "web/web_server.h"

static UsageData g_usageData;
static uint32_t  g_lastFetch  = 0;
static bool      g_firstFetch = true;
static bool      g_ntpStarted = false;

volatile bool g_forceRefresh = false;

void setup() {
    Serial.begin(115200);
    delay(500);

    settingsInit();

    if (!displayInit()) {
        Serial.println("[Display] OLED init failed — check SDA/SCL wiring");
    }
    displayShowStatus("Booting...");

    wifiStart();

    const Settings& sc = settingsGet();
    if (sc.wifiSsid[0]) {
        displayShowStatus("WiFi connecting");
        uint32_t t0 = millis();
        while (!wifiIsConnected() && millis() - t0 < 8000) delay(200);

        if (wifiIsConnected()) {
            Serial.printf("[WiFi] STA IP: %s\n", WiFi.localIP().toString().c_str());
            displayShowStatus("Syncing time...");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            setenv("TZ", "UTC0", 1);
            tzset();
            g_ntpStarted = true;
            // Wait up to 4 s for NTP sync
            uint32_t t1 = millis();
            while (time(nullptr) < 1000000000L && millis() - t1 < 4000) delay(200);
            Serial.printf("[NTP] time: %lu\n", (unsigned long)time(nullptr));
            displayShowStatus("WiFi ready");
        } else {
            displayShowStatus("AP only");
        }
    } else {
        displayShowStatus("Set WiFi in portal");
    }

    webServerStart(g_usageData);
    Serial.println("[Main] Setup complete");
}

void loop() {
    Settings& s = settingsGet();
    uint32_t  now = millis();

    // Start NTP if WiFi connected after setup
    if (!g_ntpStarted && wifiIsConnected()) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", "UTC0", 1);
        tzset();
        g_ntpStarted = true;
    }

    bool timeToFetch = g_firstFetch || (now - g_lastFetch >= s.refreshMs);

    if (timeToFetch || g_forceRefresh) {
        g_forceRefresh = false;
        g_firstFetch   = false;
        g_lastFetch    = now;

        if (!s.wifiSsid[0]) {
            displayShowError("Set WiFi SSID");
        } else if (!wifiIsConnected()) {
            displayShowError("WiFi connecting");
        } else if (!s.sessionKey[0]) {
            displayShowError("Set session key");
        } else {
            bool hadOrgId = (s.orgId[0] != '\0');

            if (apiFetch(g_usageData, s.sessionKey, s.orgId, sizeof(s.orgId))) {
                if (!hadOrgId && s.orgId[0] != '\0') {
                    settingsSave(s);
                }
                displayRender(g_usageData, s);
                Serial.printf("[Main] 5h:%d%% 7d:%d%%\n",
                    g_usageData.fiveHour.utilization,
                    g_usageData.sevenDay.utilization);
            } else {
                displayShowError("API Error");
            }
        }
    }

    displayTick();
    delay(100);
}
