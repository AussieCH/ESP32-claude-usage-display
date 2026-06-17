#pragma once
#include "../models/usage_data.h"
#include "../storage/settings.h"

bool displayInit();
void displayShowStatus(const char* msg);
void displayShowError(const char* msg);
void displayRender(const UsageData& data, const Settings& settings);
