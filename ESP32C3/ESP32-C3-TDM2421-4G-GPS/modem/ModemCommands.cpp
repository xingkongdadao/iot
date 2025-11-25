#include "ModemCommands.h"

#include <HardwareSerial.h>

#include "../config/AppConfig.h"

namespace {

HardwareSerial& modemPort() {
    return AppConfig::modemSerial();
}

}  // namespace

void sim_at_wait() {
    delay(500);
    while (modemPort().available()) {
        Serial.write(modemPort().read());
    }
}

bool sim_at_cmd(const String& cmd) {
    Serial.print("Sending command: ");
    Serial.println(cmd);
    modemPort().println(cmd);
    delay(500);
    sim_at_wait();
    return true;
}

bool sim_at_send(char c) {
    modemPort().write(c);
    return true;
}

bool sim_at_cmd_with_response(const String& cmd, String& response, uint32_t timeoutMs) {
    Serial.print("Sending command: ");
    Serial.println(cmd);
    modemPort().println(cmd);
    response = "";
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (modemPort().available()) {
            char c = modemPort().read();
            response += c;
        }
        if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) {
            break;
        }
        delay(10);
    }
    Serial.print("Received: ");
    Serial.println(response);
    return response.indexOf("OK") != -1;
}

bool waitForSubstring(const String& expect, uint32_t timeoutMs, String* response) {
    unsigned long start = millis();
    String buffer;
    while (millis() - start < timeoutMs) {
        while (modemPort().available()) {
            char c = modemPort().read();
            Serial.write(c);
            buffer += c;
            if (buffer.indexOf("ERROR") != -1) {
                if (response) {
                    *response = buffer;
                }
                return false;
            }
            if (buffer.indexOf(expect) != -1) {
                if (response) {
                    *response = buffer;
                }
                return true;
            }
        }
        delay(10);
    }
    if (response) {
        *response = buffer;
    }
    return false;
}

bool sim_at_cmd_expect(const String& cmd,
                       const String& expect,
                       uint32_t timeoutMs,
                       String* response) {
    Serial.print("Sending command: ");
    Serial.println(cmd);
    modemPort().println(cmd);
    return waitForSubstring(expect, timeoutMs, response);
}

