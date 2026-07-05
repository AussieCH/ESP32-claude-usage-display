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

    // T-Display S3 buttons: BOOT (GPIO0) = force refresh,
    // KEY (GPIO14) = cycle backlight brightness. Both active-low.
    pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
    pinMode(PIN_BTN_KEY,  INPUT_PULLUP);

    settingsInit();

    if (!displayInit()) {
        Serial.println("[Display] LCD init failed — sprite allocation needs PSRAM");
    }
    displayShowStatus("Booting...");

    wifiStart();

    const Settings& sc = settingsGet();
    if (wifiHasAnySsid(sc)) {
        displayShowStatus("WiFi connecting");
        uint32_t t0 = millis();
        while (!wifiIsConnected() && millis() - t0 < 12000) { wifiMaintain(); delay(200); }

        if (wifiIsConnected()) {
            Serial.printf("[WiFi] STA IP: %s\n", WiFi.localIP().toString().c_str());
            displayShowStatus("Syncing time...");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // Europe/Zurich (DST auto)
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

    // Keep STA connected to the strongest configured network (auto-switch on move)
    wifiMaintain();

    // Start NTP if WiFi connected after setup
    if (!g_ntpStarted && wifiIsConnected()) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // Europe/Zurich (DST auto)
        tzset();
        g_ntpStarted = true;
    }

    bool timeToFetch = g_firstFetch || (now - g_lastFetch >= s.refreshMs);

    if (timeToFetch || g_forceRefresh) {
        g_forceRefresh = false;
        g_firstFetch   = false;
        g_lastFetch    = now;

        if (!wifiHasAnySsid(s)) {
            displayShowError("Set WiFi SSID");
        } else if (!wifiIsConnected()) {
            displayShowError("WiFi connecting");
        } else if (s.proxyUrl[0]) {
            // Proxy mode: local/Tailscale usage proxy, no cookie needed
            if (apiFetchProxy(g_usageData, s.proxyUrl, s.proxyToken)) {
                displayRender(g_usageData, s);
                Serial.printf("[Main] (proxy) 5h:%d%% 7d:%d%%\n",
                    g_usageData.fiveHour.utilization,
                    g_usageData.sevenDay.utilization);
            } else {
                displayShowError("Proxy Error");
            }
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

    // Buttons (polled at loop rate; 100 ms delay below doubles as debounce)
    static bool prevBoot = true, prevKey = true;
    static const uint8_t BL_STEPS[] = { 100, 60, 25, 5 };
    static uint8_t blIdx = 0;

    bool boot = digitalRead(PIN_BTN_BOOT);
    bool key  = digitalRead(PIN_BTN_KEY);

    if (prevBoot && !boot) {          // falling edge → force refresh
        g_forceRefresh = true;
        Serial.println("[Btn] BOOT pressed — forcing refresh");
    }
    if (prevKey && !key) {            // falling edge → next brightness step
        blIdx = (blIdx + 1) % (sizeof(BL_STEPS) / sizeof(BL_STEPS[0]));
        displaySetBrightness(BL_STEPS[blIdx]);
        Serial.printf("[Btn] KEY pressed — backlight %d%%\n", BL_STEPS[blIdx]);
    }
    prevBoot = boot;
    prevKey  = key;

    displayTick();
    delay(100);
}
