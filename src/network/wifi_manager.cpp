#include "wifi_manager.h"
#include "../config/config.h"
#include "../storage/settings.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <Arduino.h>

static WiFiMulti g_multi;

bool wifiStart() {
    const Settings& s = settingsGet();
    bool hasSta = wifiHasAnySsid(s);

    WiFi.mode(hasSta ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID_DEFAULT, s.apPassword);
    Serial.printf("[WiFi] AP ready @ %s\n", WiFi.softAPIP().toString().c_str());

    if (hasSta) {
        WiFi.setAutoReconnect(true);
        int n = 0;
        for (int i = 0; i < WIFI_SLOTS; i++) {
            const char* ssid = wifiSsidAt(s, i);
            if (ssid[0]) { g_multi.addAP(ssid, wifiPassAt(s, i)); n++; }
        }
        Serial.printf("[WiFi] %d network(s) configured — connecting to the strongest in range\n", n);
    }
    return true;
}

// (Re)connect to the best configured network. Cheap when already connected;
// re-scans (blocking ~2-6 s) at most every 5 s while disconnected, so a moved
// device automatically switches to whichever saved network is now in range.
void wifiMaintain() {
    if (WiFi.status() == WL_CONNECTED) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (last && now - last < 5000) return;
    last = now;
    g_multi.run(6000);
}

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifiStop() {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
}
