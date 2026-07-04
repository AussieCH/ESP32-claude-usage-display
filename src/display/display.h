#pragma once
#include "../models/usage_data.h"
#include "../storage/settings.h"

bool displayInit();
void displayShowStatus(const char* msg);
void displayShowError(const char* msg);
void displayRender(const UsageData& data, const Settings& settings);
void displayTick();   // call from loop(); drives the corner-icon wink animation
const uint8_t* displayGetBuffer();   // raw 1024-byte SSD1306 framebuffer
