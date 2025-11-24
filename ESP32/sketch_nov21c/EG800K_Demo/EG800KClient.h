#pragma once

#include <Arduino.h>

struct GpsFix {
  double latitude = 0.0;
  double longitude = 0.0;
  float altitude = 0.0f;
  float speedKph = 0.0f;
  String timestamp;
  bool fixValid = false;
};

struct HttpRequest {
  String url;
  String payload;
  String contentType = "application/json";
};

struct MqttConfig {
  String host;
  uint16_t port = 1883;
  String clientId = "EG800KClient";
  String username;
  String password;
  String topic = "demo/eg800k";
};

class EG800KClient {
 public:
  EG800KClient(HardwareSerial& serial, Stream& debug);

  void begin(uint32_t baud, int8_t rxPin, int8_t txPin);
  bool sync(uint32_t timeoutMs = 5000);
  bool configureApn(const String& apn, const String& user = "", const String& pass = "");
  bool attachNetwork(uint8_t contextId = 1);
  bool exec(const String& command, const String& expect, uint32_t timeoutMs = 5000);

  bool httpPost(const HttpRequest& request, String& response);
  bool mqttConnect(const MqttConfig& config);
  bool mqttPublish(const String& topic, const String& payload, uint8_t qos = 0, bool retain = false);
  void mqttDisconnect();

  bool enableGps(bool enable);
  bool queryGps(GpsFix& fix);

 private:
  bool sendCommand(const String& command,
                   const String& expect,
                   uint32_t timeoutMs,
                   String* fullResponse = nullptr);
  bool waitForMatch(const String& expect, uint32_t timeoutMs, String* buffer);
  static double parseDeg(const String& raw, const String& hemisphere);

  HardwareSerial& modemSerial_;
  Stream& debugSerial_;
};

