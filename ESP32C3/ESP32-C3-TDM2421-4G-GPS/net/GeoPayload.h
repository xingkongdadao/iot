#pragma once

#include "../gps/GpsTypes.h"

String buildGeoSensorPayload(const GpsFix& fix, const char* networkSource = nullptr);

