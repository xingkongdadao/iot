#pragma once

#include <Arduino.h>

extern bool isUploading;

bool uploadSingleData(float distanceCm, time_t timestamp);
void uploadLocalData();

