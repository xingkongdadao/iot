#include "WifiUploader.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "../config/AppConfig.h"
#include "GeoPayload.h"

namespace {

WiFiClientSecure geoSecureClient;

}  // namespace

namespace WifiUploader {

bool upload(const GpsFix& fix) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, abort geoSensor upload");
        return false;
    }

    HTTPClient http;
    http.setTimeout(10000);

    String url = String(AppConfig::GEO_SENSOR_API_BASE_URL) + "/device/geoSensor/" + String(AppConfig::GEO_SENSOR_ID) +
                 "/";
    String payload = buildGeoSensorPayload(fix, "wifi");
    bool useHttps = url.startsWith("https://");
    bool beginResult = false;

    if (useHttps) {
        geoSecureClient.stop();
        geoSecureClient.setInsecure();
        geoSecureClient.setTimeout(10000);
        beginResult = http.begin(geoSecureClient, url);
    } else {
        static WiFiClient geoClient;
        geoClient.stop();
        geoClient.setTimeout(10000);
        beginResult = http.begin(geoClient, url);
    }

    if (!beginResult) {
        Serial.println("Failed to begin geoSensor request");
        return false;
    }

    Serial.print("geoSensor payload: ");
    Serial.println(payload);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", AppConfig::GEO_SENSOR_KEY);
    http.addHeader("Connection", "close");

    int httpCode = http.PATCH(payload);
    String httpError = http.errorToString(httpCode);
    Serial.printf("geoSensor PATCH -> code: %d (%s)\n", httpCode, httpError.c_str());
    if (httpCode > 0) {
        Serial.println(http.getString());
    }
    http.end();
    return httpCode >= 200 && httpCode < 300;
}

}  // namespace WifiUploader

