#pragma once
#include <stdbool.h>

bool wifiStart();         // start AP (always) + STA (if SSID configured)
void wifiStop();
bool wifiIsConnected();   // true when STA has internet
