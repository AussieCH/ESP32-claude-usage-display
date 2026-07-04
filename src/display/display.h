#pragma once
#include <stdint.h>
#include "../models/usage_data.h"
#include "../storage/settings.h"

bool displayInit();
void displayShowStatus(const char* msg);
void displayShowError(const char* msg);
void displayRender(const UsageData& data, const Settings& settings);
void displayTick();   // call from loop(); drives the corner-icon wink animation
void displaySetBrightness(uint8_t pct);   // 0-100 backlight duty
