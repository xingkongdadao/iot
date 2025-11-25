#pragma once

#include <Arduino.h>

bool saveDataToStorage(float distanceCm, time_t timestamp);
int getStoredDataCount();
bool readFirstDataFromStorage(float& distance, time_t& timestamp);
void removeFirstDataFromStorage();
int readBatchDataFromStorage(float* distances, time_t* timestamps, int maxCount);
void removeBatchDataFromStorage(int count);

