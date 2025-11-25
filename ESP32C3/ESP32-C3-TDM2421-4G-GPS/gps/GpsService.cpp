#include "GpsService.h"

#include <math.h>

#include "../modem/ModemCommands.h"
#include "../utils/StringUtils.h"

namespace {

float convertNmeaToDecimal(const String& raw) {
    if (raw.length() < 2) {
        return 0.0f;
    }
    char hemi = raw.charAt(raw.length() - 1);
    String numeric = raw.substring(0, raw.length() - 1);
    float value = numeric.toFloat();
    int degrees = static_cast<int>(value / 100.0f);
    float minutes = value - degrees * 100.0f;
    float decimal = degrees + minutes / 60.0f;
    if (hemi == 'S' || hemi == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

String buildIso8601UtcFromGps(const String& dateField, const String& timeField) {
    if (dateField.length() < 6 || timeField.length() < 6) {
        return "";
    }
    int day = dateField.substring(0, 2).toInt();
    int month = dateField.substring(2, 4).toInt();
    int year = 2000 + dateField.substring(4, 6).toInt();
    int hour = timeField.substring(0, 2).toInt();
    int minute = timeField.substring(2, 4).toInt();
    int second = timeField.substring(4, 6).toInt();
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d+00:00", year, month, day, hour, minute, second);
    return String(buffer);
}

bool parseGpsResponse(const String& raw, GpsFix& fix) {
    int tagPos = raw.indexOf("+QGPSLOC:");
    if (tagPos == -1) {
        return false;
    }
    int lineEnd = raw.indexOf("\n", tagPos);
    if (lineEnd == -1) {
        lineEnd = raw.length();
    }
    String data = raw.substring(tagPos + 10, lineEnd);
    data.trim();
    data.replace("\r", "");
    const size_t FIELD_COUNT = 11;
    String fields[FIELD_COUNT];
    if (!StringUtils::splitCsvFields(data, fields, FIELD_COUNT)) {
        return false;
    }
    fix.latitude = convertNmeaToDecimal(fields[1]);
    fix.longitude = convertNmeaToDecimal(fields[2]);
    fix.altitude = fields[4].toFloat();
    fix.speed = fields[7].toFloat();
    fix.dataAcquiredAt = buildIso8601UtcFromGps(fields[9], fields[0]);
    fix.satelliteCount = static_cast<uint8_t>(fields[10].toInt());
    return true;
}

}  // namespace

namespace GpsService {

void warmup() {
    sim_at_cmd("AT+QGPS=1");
    delay(60000);
    sim_at_cmd("AT+QGPS?");
    sim_at_cmd("AT+QGPSLOC=0");
}

bool fetchFix(GpsFix& fix) {
    String response;
    if (!sim_at_cmd_with_response("AT+QGPSLOC=0", response)) {
        return false;
    }
    return parseGpsResponse(response, fix);
}

}  // namespace GpsService

