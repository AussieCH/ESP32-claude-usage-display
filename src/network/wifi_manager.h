#pragma once
#include <stdbool.h>

bool wifiStart();         // start AP (always) + register STA networks
void wifiMaintain();      // (re)connect to the strongest configured network; call from loop
void wifiStop();
bool wifiIsConnected();   // true when STA has internet
