#include <Arduino.h>

#include "EG800KClient.h"

constexpr int kLedPin = 10;
constexpr uint32_t kBlinkOn = 800;
constexpr uint32_t kBlinkOff = 400;

constexpr int kModemRxPin = 20;  // ESP32-C3 RXD <- EG800K TXD
constexpr int kModemTxPin = 21;  // ESP32-C3 TXD -> EG800K RXD
constexpr uint32_t kModemBaud = 115200;

const char* kApn = "your.apn";
const char* kApnUser = "";
const char* kApnPass = "";

const char* kHttpUrl = "https://httpbin.org/post";

MqttConfig kMqttConfig = {
    .host = "broker.emqx.io",
    .port = 1883,
    .clientId = "eg800k-demo-client",
    .username = "",
    .password = "",
    .topic = "demo/eg800k/telemetry",
};

HardwareSerial modemSerial(1);
EG800KClient modem(modemSerial, Serial);

unsigned long lastToggle = 0;
bool ledState = false;
unsigned long lastTelemetry = 0;
const unsigned long kTelemetryIntervalMs = 15000;

void blinkTask() {
  unsigned long interval = ledState ? kBlinkOn : kBlinkOff;
  if (millis() - lastToggle >= interval) {
    ledState = !ledState;
    digitalWrite(kLedPin, ledState ? HIGH : LOW);
    lastToggle = millis();
  }
}

bool sendHttpProbe() {
  HttpRequest req;
  req.url = kHttpUrl;
  req.payload =
      "{\"device\":\"ESP32-C3\",\"module\":\"EG800K\",\"message\":\"hello http\"}";
  String response;
  if (modem.httpPost(req, response)) {
    Serial.println(F("[HTTP] POST OK"));
    Serial.println(response);
    return true;
  }
  Serial.println(F("[HTTP] POST failed"));
  return false;
}

void sendMqttTelemetry(const GpsFix& fix) {
  String payload = "{\"lat\":" + String(fix.latitude, 6) + ",\"lon\":" +
                   String(fix.longitude, 6) + ",\"alt\":" +
                   String(fix.altitude, 1) + ",\"speed\":" +
                   String(fix.speedKph, 1) + ",\"ts\":\"" + fix.timestamp + "\"}";
  if (modem.mqttPublish(kMqttConfig.topic, payload, 0, false)) {
    Serial.println(F("[MQTT] Publish OK"));
  } else {
    Serial.println(F("[MQTT] Publish failed"));
  }
}

void setup() {
  pinMode(kLedPin, OUTPUT);
  Serial.begin(115200);
  delay(200);

  Serial.println(F("=== EG800K + ESP32-C3 Demo ==="));
  modem.begin(kModemBaud, kModemRxPin, kModemTxPin);

  if (!modem.sync()) {
    Serial.println(F("Modem not responding. Check wiring/power."));
    return;
  }
  Serial.println(F("[MODEM] Ready"));

  modem.exec(F("AT+CFUN=1"), "OK", 5000);
  modem.exec(F("AT+CPIN?"), "READY", 5000);

  if (!modem.configureApn(kApn, kApnUser, kApnPass) || !modem.attachNetwork()) {
    Serial.println(F("[MODEM] Network attach failed"));
    return;
  }
  Serial.println(F("[MODEM] Network attached"));

  if (sendHttpProbe()) {
    Serial.println(F("[HTTP] Probe complete"));
  }

  if (modem.mqttConnect(kMqttConfig)) {
    Serial.println(F("[MQTT] Connected"));
  } else {
    Serial.println(F("[MQTT] Connect failed"));
  }

  if (modem.enableGps(true)) {
    Serial.println(F("[GPS] Enabled, waiting for fix..."));
  }
}

void loop() {
  blinkTask();

  static GpsFix fix;
  if (modem.queryGps(fix)) {
    Serial.print(F("[GPS] Fix "));
    Serial.print(fix.latitude, 6);
    Serial.print(F(", "));
    Serial.print(fix.longitude, 6);
    Serial.print(F(" alt "));
    Serial.print(fix.altitude, 1);
    Serial.print(F("m speed "));
    Serial.print(fix.speedKph, 1);
    Serial.print(F("km/h time "));
    Serial.println(fix.timestamp);

    if (millis() - lastTelemetry > kTelemetryIntervalMs) {
      sendMqttTelemetry(fix);
      lastTelemetry = millis();
    }
  }
}

