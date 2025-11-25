#pragma once

#include <WiFi.h>

namespace WifiManager {

void begin();
bool ensureConnected();

}  // namespace WifiManager

