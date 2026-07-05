#pragma once
#include <stdint.h>
#include "../models/usage_data.h"
#include "../storage/settings.h"

bool displayInit();
void displayShowStatus(const char* msg);
void displayShowError(const char* msg);
void displayRender(const UsageData& data, const Settings& settings);
bool displayHasData();      // true once real usage data has been rendered at least once
void displayShowStale();    // re-render the last good frame with a "stale" marker
void displayTick();   // call from loop(); drives the corner-icon wink animation
void displaySetBrightness(uint8_t pct);   // 0-100 backlight duty
