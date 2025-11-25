#pragma once

#include <Arduino.h>

extern bool timeSynced;

void syncNTPTime();
time_t getCurrentTimestamp();
String formatDateTime(time_t timestamp);

