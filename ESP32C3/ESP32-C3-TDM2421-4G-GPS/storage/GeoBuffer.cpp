#include "GeoBuffer.h"

#include "../config/AppConfig.h"
#include "../utils/StringUtils.h"

namespace {

Preferences geoPrefs;
bool geoPrefsReady = false;
GpsFix geoSensorBuffer[AppConfig::GEO_SENSOR_BUFFER_CAPACITY];
size_t geoSensorBufferStart = 0;
size_t geoSensorBufferCount = 0;

constexpr char GEO_BUFFER_PREF_NAMESPACE[] = "geoBuf";
constexpr char GEO_BUFFER_PREF_START_KEY[] = "start";
constexpr char GEO_BUFFER_PREF_COUNT_KEY[] = "count";

String geoBufferSlotKey(size_t index) {
    return String("fix") + String(static_cast<unsigned>(index));
}

String serializeGpsFix(const GpsFix& fix) {
    String data;
    data.reserve(112);
    data += String(fix.latitude, 8);
    data += ",";
    data += String(fix.longitude, 8);
    data += ",";
    data += String(fix.altitude, 2);
    data += ",";
    data += String(fix.speed, 2);
    data += ",";
    data += fix.dataAcquiredAt;
    data += ",";
    data += String(static_cast<unsigned>(fix.satelliteCount));
    return data;
}

bool deserializeGpsFix(const String& data, GpsFix& fix) {
    const size_t CURRENT_FIELD_COUNT = 6;
    String fields[CURRENT_FIELD_COUNT];
    if (StringUtils::splitCsvFields(data, fields, CURRENT_FIELD_COUNT)) {
        fix.latitude = fields[0].toFloat();
        fix.longitude = fields[1].toFloat();
        fix.altitude = fields[2].toFloat();
        fix.speed = fields[3].toFloat();
        fix.dataAcquiredAt = fields[4];
        fix.satelliteCount = static_cast<uint8_t>(fields[5].toInt());
        return true;
    }
    const size_t LEGACY_FIELD_COUNT = 5;
    String legacyFields[LEGACY_FIELD_COUNT];
    if (!StringUtils::splitCsvFields(data, legacyFields, LEGACY_FIELD_COUNT)) {
        return false;
    }
    fix.latitude = legacyFields[0].toFloat();
    fix.longitude = legacyFields[1].toFloat();
    fix.altitude = legacyFields[2].toFloat();
    fix.speed = legacyFields[3].toFloat();
    fix.dataAcquiredAt = legacyFields[4];
    fix.satelliteCount = 0;
    return true;
}

size_t geoSensorBufferIndex(size_t offset) {
    return (geoSensorBufferStart + offset) % AppConfig::GEO_SENSOR_BUFFER_CAPACITY;
}

void persistGeoSensorMetadata() {
    if (!geoPrefsReady) {
        return;
    }
    geoPrefs.putUShort(GEO_BUFFER_PREF_START_KEY, static_cast<uint16_t>(geoSensorBufferStart));
    geoPrefs.putUShort(GEO_BUFFER_PREF_COUNT_KEY, static_cast<uint16_t>(geoSensorBufferCount));
}

void persistGeoSensorSlot(size_t index) {
    if (!geoPrefsReady) {
        return;
    }
    geoPrefs.putString(geoBufferSlotKey(index).c_str(), serializeGpsFix(geoSensorBuffer[index]));
}

void clearGeoSensorSlot(size_t index) {
    if (!geoPrefsReady) {
        return;
    }
    geoPrefs.remove(geoBufferSlotKey(index).c_str());
}

void geoSensorBufferDropOldestUnsafe() {
    if (geoSensorBufferCount == 0) {
        return;
    }
    size_t dropIndex = geoSensorBufferStart;
    clearGeoSensorSlot(dropIndex);
    geoSensorBufferStart = geoSensorBufferIndex(1);
    --geoSensorBufferCount;
    persistGeoSensorMetadata();
}

void restoreBufferFromFlash() {
    size_t storedStart = geoPrefs.getUShort(GEO_BUFFER_PREF_START_KEY, 0);
    size_t storedCount = geoPrefs.getUShort(GEO_BUFFER_PREF_COUNT_KEY, 0);
    if (storedStart >= AppConfig::GEO_SENSOR_BUFFER_CAPACITY) {
        storedStart = 0;
    }
    if (storedCount > AppConfig::GEO_SENSOR_BUFFER_CAPACITY) {
        storedCount = 0;
    }
    geoSensorBufferStart = storedStart;
    geoSensorBufferCount = 0;
    bool truncated = false;
    for (size_t offset = 0; offset < storedCount; ++offset) {
        size_t index = geoSensorBufferIndex(offset);
        String raw = geoPrefs.getString(geoBufferSlotKey(index).c_str(), "");
        if (raw.isEmpty()) {
            truncated = true;
            break;
        }
        GpsFix fix;
        if (!deserializeGpsFix(raw, fix)) {
            truncated = true;
            break;
        }
        geoSensorBuffer[index] = fix;
        ++geoSensorBufferCount;
    }
    if (truncated) {
        Serial.println("Detected corrupted geo buffer entries, truncating queue");
        for (size_t offset = geoSensorBufferCount; offset < storedCount; ++offset) {
            size_t index = geoSensorBufferIndex(offset);
            clearGeoSensorSlot(index);
        }
    }
    persistGeoSensorMetadata();
    Serial.printf("Restored %u buffered fixes from flash\n", static_cast<unsigned>(geoSensorBufferCount));
}

}  // namespace

namespace GeoBuffer {

void init() {
    if (geoPrefsReady) {
        return;
    }
    if (!geoPrefs.begin(GEO_BUFFER_PREF_NAMESPACE, false)) {
        Serial.println("Failed to init geo buffer prefs, using RAM-only buffer");
        return;
    }
    geoPrefsReady = true;
    restoreBufferFromFlash();
}

bool empty() {
    return geoSensorBufferCount == 0;
}

size_t count() {
    return geoSensorBufferCount;
}

void enqueue(const GpsFix& fix) {
    if (geoSensorBufferCount == AppConfig::GEO_SENSOR_BUFFER_CAPACITY) {
        geoSensorBufferDropOldestUnsafe();
    }
    size_t insertIndex = geoSensorBufferIndex(geoSensorBufferCount);
    geoSensorBuffer[insertIndex] = fix;
    ++geoSensorBufferCount;
    persistGeoSensorSlot(insertIndex);
    persistGeoSensorMetadata();
    Serial.printf("Buffered geoSensor fix, count=%u\n", static_cast<unsigned>(geoSensorBufferCount));
}

bool peek(GpsFix& fix) {
    if (empty()) {
        return false;
    }
    fix = geoSensorBuffer[geoSensorBufferStart];
    return true;
}

void dropOldest() {
    geoSensorBufferDropOldestUnsafe();
}

}  // namespace GeoBuffer

