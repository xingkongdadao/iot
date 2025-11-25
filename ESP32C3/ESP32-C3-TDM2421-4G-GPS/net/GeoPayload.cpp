#include "GeoPayload.h"

#include <math.h>

#include "../config/AppConfig.h"

namespace {

String formatCoordinate(double value) {
    double absVal = fabs(value);
    int digitsBeforeDecimal = 1;
    if (absVal >= 1.0) {
        digitsBeforeDecimal = static_cast<int>(floor(log10(absVal))) + 1;
    }
    int decimals = 8 - digitsBeforeDecimal;
    if (decimals < 0) {
        decimals = 0;
    } else if (decimals > 6) {
        decimals = 6;
    }
    return String(value, decimals);
}

}  // namespace

String buildGeoSensorPayload(const GpsFix& fix, const char* networkSource) {
    String payload = "{";
    payload += "\"sensorId\":\"" + String(AppConfig::GEO_SENSOR_ID) + "\",";
    payload += "\"latitude\":" + formatCoordinate(fix.latitude) + ",";
    payload += "\"longitude\":" + formatCoordinate(fix.longitude) + ",";
    payload += "\"altitude\":" + String(fix.altitude, 2) + ",";
    payload += "\"speed\":" + String(fix.speed, 2) + ",";
    payload += "\"satelliteCount\":" + String(static_cast<unsigned>(fix.satelliteCount)) + ",";
    if (networkSource != nullptr && networkSource[0] != '\0') {
        payload += "\"networkSource\":\"" + String(networkSource) + "\",";
    }
    payload += "\"dataAcquiredAt\":\"" + fix.dataAcquiredAt + "\"";
    payload += "}";
    return payload;
}

