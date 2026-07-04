#include "api_service.h"
#include "../config/config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <Arduino.h>

// If the stored value already looks like a full cookie string (contains '=' or ';'),
// send it as-is. Otherwise assume it's just the sessionKey value and prepend the name.
static void buildCookieHeader(const char* sessionKey, char* buf, size_t len) {
    if (strchr(sessionKey, '=') || strchr(sessionKey, ';'))
        snprintf(buf, len, "%s", sessionKey);
    else
        snprintf(buf, len, "sessionKey=%s", sessionKey);
}

static bool doGet(const char* url, const char* cookieHeader,
                  String& responseBody) {
    WiFiClientSecure client;
    client.setInsecure();  // skip TLS cert check — personal device only

    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.printf("[API] begin failed: %s\n", url);
        return false;
    }
    http.setTimeout(10000);
    http.addHeader("Accept", "application/json");
    http.addHeader("Cookie", cookieHeader);
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");
    http.addHeader("Referer", "https://claude.ai/");
    http.addHeader("Origin", "https://claude.ai");

    int code = http.GET();
    if (code == 200) {
        responseBody = http.getString();
        http.end();
        return true;
    }
    Serial.printf("[API] HTTP %d for %s\n", code, url);
    if (code < 0) Serial.printf("[API] error: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
}

// Discover org UUID from /api/organizations — find org with "chat" capability.
static bool fetchOrgId(const char* cookieHeader, char* orgIdBuf, size_t orgIdLen) {
    String body;
    if (!doGet(CLAUDE_ORG_URL, cookieHeader, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    JsonArray orgs = doc.as<JsonArray>();
    if (orgs.isNull() || orgs.size() == 0) return false;

    const char* found = nullptr;
    for (JsonObject org : orgs) {
        for (JsonVariant cap : org["capabilities"].as<JsonArray>()) {
            if (strcmp(cap.as<const char*>() ?: "", "chat") == 0) {
                found = org["uuid"];
                break;
            }
        }
        if (found) break;
    }
    if (!found) found = orgs[0]["uuid"];
    if (!found) return false;

    snprintf(orgIdBuf, orgIdLen, "%s", found);
    Serial.printf("[API] org: %s\n", orgIdBuf);
    return true;
}

static void parseBlock(JsonObject obj, UsageBlock& block) {
    block.available = !obj.isNull();
    if (!block.available) { block.utilization = 0; block.resetsAt[0] = '\0'; return; }

    // API returns utilization as a 0.0–1.0 fraction (e.g. 0.20 = 20%)
    float f = 0.0f;
    if (!obj["utilization"].isNull())          f = obj["utilization"].as<float>();
    else if (!obj["utilization_pct"].isNull()) f = obj["utilization_pct"].as<float>();
    block.utilization = (uint8_t)min((int)lroundf(f), 100);

    const char* rst = nullptr;
    if (!obj["resets_at"].isNull())  rst = obj["resets_at"].as<const char*>();
    else if (!obj["reset_at"].isNull()) rst = obj["reset_at"].as<const char*>();
    snprintf(block.resetsAt, sizeof(block.resetsAt), "%s", rst ? rst : "");
}

bool apiFetch(UsageData& out, const char* sessionKey, char* orgId, size_t orgIdLen) {
    out.valid = false;

    if (!sessionKey || !sessionKey[0]) {
        Serial.println("[API] no session key");
        return false;
    }

    char cookie[544];
    buildCookieHeader(sessionKey, cookie, sizeof(cookie));

    if (!orgId[0]) {
        if (!fetchOrgId(cookie, orgId, orgIdLen)) {
            Serial.println("[API] org discovery failed");
            return false;
        }
    }

    char url[160];
    snprintf(url, sizeof(url), CLAUDE_USAGE_FMT, orgId);

    String body;
    if (!doGet(url, cookie, body)) {
        // 401/403 → stale org ID, force re-discovery next time
        orgId[0] = '\0';
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[API] usage JSON parse error");
        return false;
    }

    parseBlock(doc["five_hour"].as<JsonObject>(), out.fiveHour);
    parseBlock(doc["seven_day"].as<JsonObject>(), out.sevenDay);
    parseBlock(doc["seven_day_opus"].as<JsonObject>(), out.sevenDayOpus);

    // Current model: the model-scoped limit carries its display name
    out.model[0] = '\0';
    for (JsonObject lim : doc["limits"].as<JsonArray>()) {
        const char* name = lim["scope"]["model"]["display_name"];
        if (name && name[0]) {
            snprintf(out.model, sizeof(out.model), "%s", name);
            break;
        }
    }

    unsigned long sec = millis() / 1000;
    snprintf(out.timestamp, sizeof(out.timestamp), "T+%02lu:%02lu:%02lu",
             sec / 3600, (sec % 3600) / 60, sec % 60);

    out.valid = true;
    return true;
}
