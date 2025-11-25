
#include <Arduino.h>
#include <WiFi.h>

#include "config/AppConfig.h"
#include "gps/GpsService.h"
#include "modem/ModemCommands.h"
#include "net/GeoUploader.h"
#include "wifi/WifiManager.h"

namespace {

void initializeModemDiagnostics() {
    Serial.println("Checking AT command...");
    sim_at_cmd("AT");
    Serial.println("Getting product info...");
    sim_at_cmd("ATI");
    Serial.println("Checking SIM status...");
    sim_at_cmd("AT+CPIN?");
    Serial.println("Checking signal quality...");
    sim_at_cmd("AT+CSQ");
    Serial.println("Getting IMSI...");
    sim_at_cmd("AT+CIMI");
}

void forwardUsbToModem() {
    if (Serial.available()) {
        AppConfig::modemSerial().write(Serial.read());
    }
}

}  // namespace

void setup() {
    pinMode(AppConfig::MCU_SIM_EN_PIN, OUTPUT);
    digitalWrite(AppConfig::MCU_SIM_EN_PIN, HIGH);
    delay(500);

    Serial.begin(115200);
    pinMode(AppConfig::MCU_LED, OUTPUT);
    digitalWrite(AppConfig::MCU_LED, HIGH);
    Serial.println("\n\n\n\n-----------------------\nSystem started!!!!");
    delay(8000);

    GeoUploader::init();
    WifiManager::begin();
    Serial.printf("Connecting to %s\n", AppConfig::WIFI_SSID);
    if (!WifiManager::ensureConnected()) {
        Serial.println("WiFi unavailable, will retry in loop");
    }

    AppConfig::modemSerial().begin(
        AppConfig::MCU_SIM_BAUDRATE, SERIAL_8N1, AppConfig::MCU_SIM_RX_PIN, AppConfig::MCU_SIM_TX_PIN);
    initializeModemDiagnostics();
    WifiManager::ensureConnected();
    GpsService::warmup();
}

void loop() {
    forwardUsbToModem();
    WifiManager::ensureConnected();
    GeoUploader::flushBuffer();
    GeoUploader::handleUpdate();
}
