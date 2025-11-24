#include "EG800KClient.h"

namespace {

const uint32_t kDefaultTimeout = 8000;
const uint32_t kNetworkAttachTimeout = 30000;

bool isHemisphereChar(char c) {
  return c == 'N' || c == 'S' || c == 'E' || c == 'W';
}

String trimResponse(const String& input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\r') {
      continue;
    }
    out += c;
  }
  return out;
}

String nextToken(const String& src, char delimiter, int index) {
  int start = 0;
  int end = -1;
  int current = 0;
  while (current <= index) {
    start = end + 1;
    end = src.indexOf(delimiter, start);
    if (end == -1) {
      end = src.length();
    }
    if (current == index) {
      return src.substring(start, end);
    }
    ++current;
  }
  return "";
}

bool isHemisphereField(const String& token) {
  return token.length() == 1 && isHemisphereChar(token[0]);
}

void normalizeCoordinate(String& token, String& hemisphere) {
  hemisphere = "";
  if (!token.length()) {
    return;
  }
  char last = token[token.length() - 1];
  if (isHemisphereChar(last)) {
    hemisphere = String(last);
    token.remove(token.length() - 1);
  }
}

}  // namespace

EG800KClient::EG800KClient(HardwareSerial& serial, Stream& debug)
    : modemSerial_(serial), debugSerial_(debug) {}

void EG800KClient::begin(uint32_t baud, int8_t rxPin, int8_t txPin) {
  modemSerial_.begin(baud, SERIAL_8N1, rxPin, txPin);
}

bool EG800KClient::sync(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    modemSerial_.println(F("AT"));
    if (waitForMatch("OK", 500, nullptr)) {
      sendCommand(F("ATE0"), "OK", kDefaultTimeout);
      return true;
    }
    delay(200);
  }
  return false;
}

bool EG800KClient::configureApn(const String& apn, const String& user, const String& pass) {
  String cmd = "AT+QICSGP=1,1,\"" + apn + "\",\"" + user + "\",\"" + pass + "\",1";
  return sendCommand(cmd, "OK", kDefaultTimeout);
}

bool EG800KClient::attachNetwork(uint8_t contextId) {
  String cmd = "AT+QIACT=" + String(contextId);
  if (!sendCommand(cmd, "OK", kNetworkAttachTimeout)) {
    return false;
  }
  return sendCommand("AT+QIACT?", "+QIACT:", kDefaultTimeout);
}

bool EG800KClient::httpPost(const HttpRequest& request, String& response) {
  if (!sendCommand(F("AT+QHTTPCFG=\"contextid\",1"), "OK", kDefaultTimeout)) {
    return false;
  }
  if (!sendCommand(F("AT+QHTTPCFG=\"responseheader\",1"), "OK", kDefaultTimeout)) {
    return false;
  }
  String contentTypeCmd =
      "AT+QHTTPCFG=\"contenttype\",\"" + request.contentType + "\"";
  if (!sendCommand(contentTypeCmd, "OK", kDefaultTimeout)) {
    return false;
  }

  String urlCmd = "AT+QHTTPURL=" + String(request.url.length()) + ",80";
  if (!sendCommand(urlCmd, "CONNECT", kDefaultTimeout)) {
    return false;
  }
  modemSerial_.print(request.url);
  if (!waitForMatch("OK", kDefaultTimeout, nullptr)) {
    return false;
  }

  String postCmd = "AT+QHTTPPOST=" + String(request.payload.length()) + ",60,60";
  if (!sendCommand(postCmd, "CONNECT", kDefaultTimeout)) {
    return false;
  }
  modemSerial_.print(request.payload);
  if (!waitForMatch("+QHTTPPOST:", kDefaultTimeout, nullptr)) {
    return false;
  }

  if (!sendCommand(F("AT+QHTTPREAD=80"), "+QHTTPREAD:", kDefaultTimeout, &response)) {
    return false;
  }
  response = trimResponse(response);
  return true;
}

bool EG800KClient::exec(const String& command, const String& expect, uint32_t timeoutMs) {
  return sendCommand(command, expect, timeoutMs);
}

bool EG800KClient::mqttConnect(const MqttConfig& config) {
  String openCmd =
      "AT+QMTOPEN=0,\"" + config.host + "\"," + String(config.port);
  if (!sendCommand(openCmd, "+QMTOPEN: 0,0", kNetworkAttachTimeout)) {
    return false;
  }

  String connCmd = "AT+QMTCONN=0,\"" + config.clientId + "\"";
  if (config.username.length()) {
    connCmd += ",\"" + config.username + "\",\"" + config.password + "\"";
  }
  if (!sendCommand(connCmd, "+QMTCONN: 0,0,0", kDefaultTimeout)) {
    return false;
  }

  if (config.topic.length()) {
    return mqttPublish(config.topic, F("{\"status\":\"online\"}"), 0, true);
  }
  return true;
}

bool EG800KClient::mqttPublish(const String& topic,
                               const String& payload,
                               uint8_t qos,
                               bool retain) {
  String cmd = "AT+QMTPUB=0,0," + String(qos) + "," + String(retain ? 1 : 0) +
               ",\"" + topic + "\"";
  if (!sendCommand(cmd, ">", kDefaultTimeout)) {
    return false;
  }
  modemSerial_.print(payload);
  modemSerial_.write(0x1A);  // CTRL+Z to finish
  return waitForMatch("+QMTPUB: 0,0,0", kDefaultTimeout, nullptr);
}

void EG800KClient::mqttDisconnect() {
  sendCommand(F("AT+QMTDISC=0"), "OK", kDefaultTimeout);
  sendCommand(F("AT+QMTCLOSE=0"), "OK", kDefaultTimeout);
}

bool EG800KClient::enableGps(bool enable) {
  if (enable) {
    if (!sendCommand(F("AT+QGPS=1"), "OK", kDefaultTimeout)) {
      return false;
    }
    if (!sendCommand(F("AT+QGPSCFG=\"gnssconfig\",31"), "OK", kDefaultTimeout)) {
      return false;
    }
    String status;
    if (sendCommand(F("AT+QGPS?"), "+QGPS:", kDefaultTimeout, &status)) {
      status = trimResponse(status);
      debugSerial_.print(F("[GPS] 状态 "));
      debugSerial_.println(status);
    }
    return true;
  }
  return sendCommand(F("AT+QGPSEND"), "OK", kDefaultTimeout);
}

bool EG800KClient::queryGps(GpsFix& fix) {
  String response;
  if (!sendCommand(F("AT+QGPSLOC=0"), "+QGPSLOC:", kDefaultTimeout, &response)) {
    return false;
  }

  int idx = response.indexOf("+QGPSLOC: ");
  if (idx == -1) {
    return false;
  }
  String payload = response.substring(idx + 10);
  payload.trim();
  int okPos = payload.indexOf("OK");
  if (okPos != -1) {
    payload = payload.substring(0, okPos);
  }
  payload.trim();
  if (payload.length()) {
    debugSerial_.print(F("[GPS] 原始数据 "));
    debugSerial_.println(payload);
  }

  // Expected tokens (per AT+QGPSLOC): <utc>,<lat>,<lon>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>
  String tokens[16];
  size_t tokenCount = 0;
  int start = 0;
  while (tokenCount < 16 && start < payload.length()) {
    int comma = payload.indexOf(',', start);
    if (comma == -1) {
      tokens[tokenCount++] = payload.substring(start);
      break;
    }
    tokens[tokenCount++] = payload.substring(start, comma);
    start = comma + 1;
  }
  if (tokenCount < 11) {
    return false;
  }

  size_t cursor = 0;
  const String utcField = tokens[cursor++];

  if (cursor >= tokenCount) {
    return false;
  }
  String latToken = tokens[cursor++];
  String latDir;
  normalizeCoordinate(latToken, latDir);
  if (!latDir.length() && cursor < tokenCount && isHemisphereField(tokens[cursor])) {
    latDir = tokens[cursor++];
  }

  if (cursor >= tokenCount) {
    return false;
  }
  String lonToken = tokens[cursor++];
  String lonDir;
  normalizeCoordinate(lonToken, lonDir);
  if (!lonDir.length() && cursor < tokenCount && isHemisphereField(tokens[cursor])) {
    lonDir = tokens[cursor++];
  }

  if (!latDir.length() || !lonDir.length()) {
    return false;
  }

  double lat = parseDeg(latToken, latDir);
  double lon = parseDeg(lonToken, lonDir);
  fix.latitude = lat;
  fix.longitude = lon;

  if (cursor >= tokenCount) {
    return false;
  }
  // hdop (not stored, but advance cursor)
  cursor++;
  if (cursor >= tokenCount) {
    return false;
  }
  fix.altitude = tokens[cursor++].toFloat();
  if (cursor >= tokenCount) {
    return false;
  }
  const int fixIndicator = tokens[cursor++].toInt();
  if (cursor >= tokenCount) {
    return false;
  }
  // cog (ignored but advance)
  cursor++;
  if (cursor < tokenCount) {
    fix.speedKph = tokens[cursor++].toFloat();
  }
  if (cursor < tokenCount) {
    // skip speed in knots
    cursor++;
  }

  String dateField;
  String trailingUtc = utcField;
  if (cursor < tokenCount) {
    dateField = tokens[cursor++];
  }
  if (cursor < tokenCount) {
    // Some firmware append UTC again before nsat
    if ((tokenCount - cursor) > 1) {
      trailingUtc = tokens[cursor++];
    }
    if (cursor < tokenCount) {
      cursor++;  // nsat, unused
    }
  }

  fix.timestamp = dateField.length() ? (dateField + " " + trailingUtc) : trailingUtc;
  fix.fixValid = fixIndicator >= 1;
  return fix.fixValid;
}

bool EG800KClient::sendCommand(const String& command,
                               const String& expect,
                               uint32_t timeoutMs,
                               String* fullResponse) {
  modemSerial_.println(command);
  return waitForMatch(expect, timeoutMs, fullResponse);
}

bool EG800KClient::waitForMatch(const String& expect,
                                uint32_t timeoutMs,
                                String* buffer) {
  unsigned long start = millis();
  String response;
  while (millis() - start < timeoutMs) {
    while (modemSerial_.available()) {
      char c = modemSerial_.read();
      response += c;
      debugSerial_.print(c);
      if (response.endsWith(expect)) {
        if (buffer) {
          *buffer = response;
        }
        return true;
      }
    }
  }
  if (buffer) {
    *buffer = response;
  }
  return false;
}

double EG800KClient::parseDeg(const String& raw, const String& hemisphere) {
  if (!raw.length()) {
    return 0.0;
  }
  double val = raw.toDouble();
  int degrees = static_cast<int>(val / 100);
  double minutes = val - (degrees * 100);
  double decimal = degrees + (minutes / 60.0);
  if (hemisphere == "S" || hemisphere == "W") {
    decimal *= -1.0;
  }
  return decimal;
}

