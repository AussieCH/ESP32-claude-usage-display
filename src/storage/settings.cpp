#include "settings.h"
#include "../config/config.h"
#include <Preferences.h>
#include <string.h>

static Settings    g_settings;
static Preferences g_prefs;

static void applyDefaults(Settings& s) {
    memset(&s, 0, sizeof(s));
    snprintf(s.apPassword, sizeof(s.apPassword), "%s", AP_PASS_DEFAULT);
    s.refreshMs      = REFRESH_MS_DEFAULT;
    s.showUsagePct   = true;
    s.showProgressBar = true;
    s.show7dPct      = true;
    s.show7dOpus     = false;
    s.showResetTime  = true;
    s.schemaVersion  = PREFS_VER;
}

void settingsInit() {
    g_prefs.begin(PREFS_NS, false);
    uint8_t ver = g_prefs.getUChar("ver", 0);
    if (ver != PREFS_VER) {
        applyDefaults(g_settings);
        settingsSave(g_settings);
        return;
    }
    g_prefs.getBytes("data", &g_settings, sizeof(g_settings));
}

Settings& settingsGet() {
    return g_settings;
}

void settingsSave(const Settings& s) {
    g_settings = s;
    g_prefs.putUChar("ver", PREFS_VER);
    g_prefs.putBytes("data", &s, sizeof(s));
}

void settingsReset() {
    applyDefaults(g_settings);
    settingsSave(g_settings);
}

// ── WiFi slots ───────────────────────────────────────────────────────
// Slot 0 maps to the original wifiSsid/wifiPassword fields; slots 1-3 to
// the appended wifiSsidN[]/wifiPasswordN[] arrays.

const char* wifiSsidAt(const Settings& s, int i) {
    if (i <= 0) return s.wifiSsid;
    if (i < WIFI_SLOTS) return s.wifiSsidN[i - 1];
    return "";
}

const char* wifiPassAt(const Settings& s, int i) {
    if (i <= 0) return s.wifiPassword;
    if (i < WIFI_SLOTS) return s.wifiPasswordN[i - 1];
    return "";
}

void wifiSetSsidAt(Settings& s, int i, const char* ssid) {
    char* dst = (i <= 0) ? s.wifiSsid
              : (i < WIFI_SLOTS ? s.wifiSsidN[i - 1] : nullptr);
    if (dst) snprintf(dst, 64, "%s", ssid ? ssid : "");
}

void wifiSetPassAt(Settings& s, int i, const char* pass) {
    char* dst = (i <= 0) ? s.wifiPassword
              : (i < WIFI_SLOTS ? s.wifiPasswordN[i - 1] : nullptr);
    if (dst) snprintf(dst, 64, "%s", pass ? pass : "");
}

bool wifiHasAnySsid(const Settings& s) {
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (wifiSsidAt(s, i)[0]) return true;
    return false;
}
