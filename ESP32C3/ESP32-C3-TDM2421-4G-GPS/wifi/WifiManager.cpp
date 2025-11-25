#include "WifiManager.h"

#include "../config/AppConfig.h"

namespace {

unsigned long wifiNextRetryAt = 0;

const char* wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:
            return "IDLE";
        case WL_NO_SSID_AVAIL:
            return "SSID_UNAVAILABLE";
        case WL_SCAN_COMPLETED:
            return "SCAN_COMPLETED";
        case WL_CONNECTED:
            return "CONNECTED";
        case WL_CONNECT_FAILED:
            return "CONNECT_FAILED";
        case WL_CONNECTION_LOST:
            return "CONNECTION_LOST";
        case WL_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}

void configureWifiStack() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
}

}  // namespace

namespace WifiManager {

void begin() {
    configureWifiStack();
}

bool ensureConnected() {
    if (!AppConfig::WIFI_ENABLED) {
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    if (wifiNextRetryAt != 0 && millis() < wifiNextRetryAt) {
        return false;
    }

    configureWifiStack();
    for (uint8_t attempt = 1; attempt <= AppConfig::WIFI_MAX_ATTEMPTS; ++attempt) {
        Serial.printf("WiFi connect attempt %u/%u\n", attempt, AppConfig::WIFI_MAX_ATTEMPTS);
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.begin(AppConfig::WIFI_SSID, AppConfig::WIFI_PASSWORD);
        wl_status_t result =
            static_cast<wl_status_t>(WiFi.waitForConnectResult(AppConfig::WIFI_CONNECT_TIMEOUT_MS));
        if (result == WL_CONNECTED) {
            Serial.println("WiFi connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("RSSI: ");
            Serial.println(WiFi.RSSI());
            Serial.print("MAC Address: ");
            Serial.println(WiFi.macAddress());
            wifiNextRetryAt = 0;
            return true;
        }
        Serial.printf("WiFi connect failed -> status: %s (%d)\n", wifiStatusToString(result), result);
        delay(1000);
    }

    Serial.println("WiFi connection timeout after max attempts");
    wifiNextRetryAt = millis() + AppConfig::WIFI_RETRY_COOLDOWN_MS;
    return false;
}

}  // namespace WifiManager

