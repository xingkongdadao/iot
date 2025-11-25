#include "GeoUploader.h"

#include <WiFi.h>

#include "../cellular/CellularClient.h"
#include "../config/AppConfig.h"
#include "../gps/GpsService.h"
#include "../storage/GeoBuffer.h"
#include "../wifi/WifiManager.h"
#include "WifiUploader.h"

namespace {

unsigned long lastGeoSensorPush = 0;
int geoSensorBackoffStage = -1;
unsigned long geoSensorNextRetryAt = 0;

bool uploadGeoSensor(const GpsFix& fix) {
    if (WiFi.status() == WL_CONNECTED) {
        if (WifiUploader::upload(fix)) {
            return true;
        }
        Serial.println("WiFi upload failed, trying cellular fallback");
    }
    return CellularClient::upload(fix);
}

bool geoSensorUploadReady() {
    return geoSensorBackoffStage < 0 || millis() >= geoSensorNextRetryAt;
}

void geoSensorRecordUploadFailure() {
    if (geoSensorBackoffStage < 0) {
        geoSensorBackoffStage = 0;
    } else if (geoSensorBackoffStage < static_cast<int>(AppConfig::GEO_SENSOR_BACKOFF_STAGE_COUNT) - 1) {
        ++geoSensorBackoffStage;
    }
    unsigned long delayMs = AppConfig::GEO_SENSOR_BACKOFF_DELAYS_MS[geoSensorBackoffStage];
    geoSensorNextRetryAt = millis() + delayMs;
    Serial.printf("geoSensor upload failed, will retry after %lu ms\n", delayMs);
}

void geoSensorRecordUploadSuccess() {
    geoSensorBackoffStage = -1;
    geoSensorNextRetryAt = 0;
}

}  // namespace

namespace GeoUploader {

void init() {
    GeoBuffer::init();
    geoSensorBackoffStage = -1;
    geoSensorNextRetryAt = 0;
    lastGeoSensorPush = 0;
}

void flushBuffer() {
    if (!geoSensorUploadReady()) {
        return;
    }
    while (!GeoBuffer::empty()) {
        GpsFix next;
        if (!GeoBuffer::peek(next)) {
            break;
        }
        if (!uploadGeoSensor(next)) {
            Serial.println("Buffered geoSensor upload failed, will retry later");
            geoSensorRecordUploadFailure();
            break;
        }
        geoSensorRecordUploadSuccess();
        GeoBuffer::dropOldest();
        Serial.printf("Buffered geoSensor upload success, remaining=%u\n",
                      static_cast<unsigned>(GeoBuffer::count()));
    }
}

void handleUpdate() {
    unsigned long now = millis();
    if (lastGeoSensorPush != 0 && now - lastGeoSensorPush < AppConfig::GEO_SENSOR_UPLOAD_INTERVAL_MS) {
        return;
    }
    lastGeoSensorPush = now;
    Serial.println("handleGeoSensorUpdate triggered");
    GpsFix fix;
    if (!GpsService::fetchFix(fix)) {
        Serial.println("Failed to acquire GPS fix");
        return;
    }
    if (!geoSensorUploadReady()) {
        Serial.println("geoSensor upload postponed by backoff, buffering");
        GeoBuffer::enqueue(fix);
        return;
    }
    if (!GeoBuffer::empty()) {
        GeoBuffer::enqueue(fix);
        flushBuffer();
        return;
    }
    if (!uploadGeoSensor(fix)) {
        Serial.println("Immediate geoSensor upload failed, buffering");
        geoSensorRecordUploadFailure();
        GeoBuffer::enqueue(fix);
    } else {
        geoSensorRecordUploadSuccess();
        Serial.println("geoSensor upload success");
    }
}

}  // namespace GeoUploader

