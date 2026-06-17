#include "wifi_manager.h"
#include "../config/config.h"
#include "../storage/settings.h"
#include <WiFi.h>
#include <Arduino.h>

bool wifiStart() {
    const Settings& s = settingsGet();
    bool hasSta = (s.wifiSsid[0] != '\0');

    WiFi.mode(hasSta ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID_DEFAULT, s.apPassword);
    Serial.printf("[WiFi] AP ready @ %s\n", WiFi.softAPIP().toString().c_str());

    if (hasSta) {
        WiFi.setAutoReconnect(true);
        WiFi.begin(s.wifiSsid, s.wifiPassword);
        Serial.printf("[WiFi] STA connecting to %s\n", s.wifiSsid);
    }

    return true;
}

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifiStop() {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
}
