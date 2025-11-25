#pragma once

#include <Arduino.h>

struct GpsFix {
    float latitude = 0.0f;
    float longitude = 0.0f;
    float altitude = 0.0f;
    float speed = 0.0f;
    uint8_t satelliteCount = 0;
    String dataAcquiredAt;
};

