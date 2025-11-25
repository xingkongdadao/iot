#include "CellularClient.h"

#include <cstring>

#include "../config/AppConfig.h"
#include "../modem/ModemCommands.h"
#include "../net/GeoPayload.h"
#include "../net/UrlParser.h"

namespace {

bool cellularContextReady = false;
bool cellularSocketOpen = false;
unsigned long lastCellularReadyCheck = 0;

bool qiactResponseHasContext(const String& response) {
    String needle = "+QIACT: " + String(AppConfig::CELL_CONTEXT_ID) + ",";
    return response.indexOf(needle) != -1;
}

bool waitForSimReady(uint32_t timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        String response;
        if (sim_at_cmd_with_response("AT+CPIN?", response, 2000) && response.indexOf("READY") != -1) {
            Serial.println("SIM ready");
            return true;
        }
        delay(1000);
    }
    Serial.println("SIM not ready before timeout");
    return false;
}

bool registrationIndicatesAttached(const String& response) {
    return response.indexOf("0,1") != -1 || response.indexOf("0,5") != -1;
}

bool waitForCellularRegistration(uint32_t timeoutMs) {
    const char* REG_COMMANDS[] = {"AT+CREG?", "AT+CGREG?", "AT+CEREG?"};
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        for (size_t i = 0; i < sizeof(REG_COMMANDS) / sizeof(REG_COMMANDS[0]); ++i) {
            String response;
            if (sim_at_cmd_with_response(REG_COMMANDS[i], response, 3000) && registrationIndicatesAttached(response)) {
                Serial.printf("Network attached via %s -> %s\n", REG_COMMANDS[i], response.c_str());
                return true;
            }
        }
        delay(AppConfig::CELL_REG_CHECK_INTERVAL_MS);
    }
    Serial.println("Network registration timeout");
    return false;
}

void cellularCloseSocket() {
    if (!cellularSocketOpen) {
        return;
    }
    String closeCmd = "AT+QICLOSE=" + String(AppConfig::CELL_SOCKET_ID);
    String response;
    sim_at_cmd_with_response(closeCmd, response, 5000);
    waitForSubstring("+QIURC: \"closed\"," + String(AppConfig::CELL_SOCKET_ID), 2000, nullptr);
    cellularSocketOpen = false;
}

bool cellularOpenSocket(const ParsedUrl& parsed) {
    cellularCloseSocket();
    String openCmd = "AT+QIOPEN=" + String(AppConfig::CELL_CONTEXT_ID) + "," + String(AppConfig::CELL_SOCKET_ID) +
                     ",\"TCP\",\"" + parsed.host + "\"," + String(parsed.port) + ",0,1";
    String response;
    if (!sim_at_cmd_with_response(openCmd, response, AppConfig::CELL_SOCKET_OP_TIMEOUT_MS)) {
        Serial.println("AT+QIOPEN command failed");
        return false;
    }
    if (!waitForSubstring("+QIOPEN: " + String(AppConfig::CELL_SOCKET_ID) + ",0",
                          AppConfig::CELL_SOCKET_OP_TIMEOUT_MS,
                          nullptr)) {
        Serial.println("Socket open URC not received");
        return false;
    }
    cellularSocketOpen = true;
    return true;
}

bool cellularSendRequest(const String& request) {
    if (!sim_at_cmd_expect("AT+QISEND=" + String(AppConfig::CELL_SOCKET_ID), ">", 5000, nullptr)) {
        Serial.println("QISEND prompt not received");
        return false;
    }
    AppConfig::modemSerial().print(request);
    AppConfig::modemSerial().write(0x1A);
    if (!waitForSubstring("SEND OK", AppConfig::CELL_SOCKET_OP_TIMEOUT_MS, nullptr)) {
        Serial.println("SEND OK not received");
        return false;
    }
    return true;
}

bool waitForCellularRecv(uint32_t timeoutMs) {
    String marker = "+QIURC: \"recv\"," + String(AppConfig::CELL_SOCKET_ID);
    return waitForSubstring(marker, timeoutMs, nullptr);
}

bool extractQirdPayload(const String& chunk, int& reportedLen, String& payload) {
    int marker = chunk.indexOf("+QIRD:");
    if (marker == -1) {
        reportedLen = 0;
        payload = "";
        return false;
    }
    int lenStart = marker + 7;
    while (lenStart < chunk.length() && (chunk[lenStart] == ' ' || chunk[lenStart] == '\t')) {
        ++lenStart;
    }
    int lenEnd = chunk.indexOf("\n", lenStart);
    if (lenEnd == -1) {
        reportedLen = 0;
        payload = "";
        return false;
    }
    String lenStr = chunk.substring(lenStart, lenEnd);
    lenStr.trim();
    reportedLen = lenStr.toInt();
    int dataStart = chunk.indexOf("\n", lenEnd + 1);
    if (dataStart == -1) {
        payload = "";
        return false;
    }
    int okPos = chunk.lastIndexOf("\r\nOK");
    if (okPos == -1) {
        okPos = chunk.length();
    }
    payload = chunk.substring(dataStart + 1, okPos);
    return true;
}

bool readCellularHttpResponse(String& httpResponse) {
    httpResponse = "";
    for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        String chunk;
        String cmd = "AT+QIRD=" + String(AppConfig::CELL_SOCKET_ID) + "," + String(AppConfig::CELL_HTTP_READ_CHUNK);
        if (!sim_at_cmd_with_response(cmd, chunk, AppConfig::CELL_SOCKET_OP_TIMEOUT_MS)) {
            return false;
        }
        int reportedLen = 0;
        String payload;
        if (!extractQirdPayload(chunk, reportedLen, payload)) {
            if (chunk.indexOf("+QIRD: 0") != -1) {
                break;
            }
            continue;
        }
        if (reportedLen <= 0 && payload.length() == 0) {
            break;
        }
        httpResponse += payload;
        if (reportedLen < AppConfig::CELL_HTTP_READ_CHUNK) {
            break;
        }
    }
    return httpResponse.length() > 0;
}

int parseHttpStatusCode(const String& httpResponse) {
    int start = httpResponse.indexOf("HTTP/");
    if (start == -1) {
        return -1;
    }
    int space = httpResponse.indexOf(' ', start);
    if (space == -1 || space + 4 > httpResponse.length()) {
        return -1;
    }
    String code = httpResponse.substring(space + 1, space + 4);
    return code.toInt();
}

}  // namespace

namespace CellularClient {

bool ensureReady() {
    if (AppConfig::CELL_APN[0] == '\0') {
        return false;
    }
    if (cellularContextReady && millis() - lastCellularReadyCheck < AppConfig::CELL_READY_REFRESH_MS) {
        return true;
    }
    String response;
    if (!sim_at_cmd_with_response("AT", response, 2000)) {
        Serial.println("Cellular module not responding to AT");
        return false;
    }
    sim_at_cmd_with_response("ATE0", response, 2000);
    if (!sim_at_cmd_with_response("AT+CFUN=1", response, 10000)) {
        Serial.println("Failed to set CFUN=1");
        return false;
    }
    sim_at_cmd_with_response("AT+QCFG=\"roamservice\",2", response, 5000);
    if (!waitForSimReady(AppConfig::CELL_SIM_READY_TIMEOUT_MS)) {
        return false;
    }
    if (!waitForCellularRegistration(AppConfig::CELL_ATTACH_TIMEOUT_MS)) {
        return false;
    }
    bool contextActive = false;
    if (sim_at_cmd_with_response("AT+QIACT?", response, 5000)) {
        contextActive = qiactResponseHasContext(response);
    }
    if (!contextActive) {
        String deactivateCmd = "AT+QIDEACT=" + String(AppConfig::CELL_CONTEXT_ID);
        sim_at_cmd_with_response(deactivateCmd, response, 10000);
        String pdpCmd =
            "AT+CGDCONT=" + String(AppConfig::CELL_CONTEXT_ID) + ",\"IP\",\"" + String(AppConfig::CELL_APN) + "\"";
        if (!sim_at_cmd_with_response(pdpCmd, response, 5000)) {
            Serial.println("Failed to set PDP context");
            return false;
        }
        String apnCmd = "AT+QICSGP=" + String(AppConfig::CELL_CONTEXT_ID) + ",1,\"" + String(AppConfig::CELL_APN) +
                        "\",\"" + String(AppConfig::CELL_APN_USER) + "\",\"" + String(AppConfig::CELL_APN_PASS) +
                        "\",1";
        if (!sim_at_cmd_with_response(apnCmd, response, 5000)) {
            Serial.println("Failed to configure APN");
            return false;
        }
        String actCmd = "AT+QIACT=" + String(AppConfig::CELL_CONTEXT_ID);
        if (!sim_at_cmd_with_response(actCmd, response, AppConfig::CELL_ATTACH_TIMEOUT_MS)) {
            Serial.println("Failed to activate PDP context");
            return false;
        }
        if (!sim_at_cmd_with_response("AT+QIACT?", response, 5000) || !qiactResponseHasContext(response)) {
            Serial.println("PDP context not active after QIACT");
            return false;
        }
    }
    cellularContextReady = true;
    lastCellularReadyCheck = millis();
    Serial.println("Cellular context ready");
    return true;
}

bool upload(const GpsFix& fix) {
    if (AppConfig::CELL_APN[0] == '\0') {
        Serial.println("CELL_APN not configured, skip cellular upload");
        return false;
    }
    String fullUrl =
        String(AppConfig::GEO_SENSOR_API_BASE_URL) + "/device/geoSensor/" + String(AppConfig::GEO_SENSOR_ID) + "/";
    ParsedUrl parsed;
    if (!parseUrl(fullUrl, parsed)) {
        Serial.println("Failed to parse geoSensor URL");
        return false;
    }
    if (parsed.https) {
        Serial.println("Cellular fallback currently supports HTTP only");
        return false;
    }
    if (!ensureReady()) {
        return false;
    }
    if (!cellularOpenSocket(parsed)) {
        return false;
    }
    String payload = buildGeoSensorPayload(fix, "4g");
    String hostHeader = parsed.host;
    if (parsed.port != 80 && parsed.port != 443) {
        hostHeader += ":" + String(parsed.port);
    }
    String request = "PATCH " + parsed.path + " HTTP/1.1\r\n";
    request += "Host: " + hostHeader + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "X-API-Key: " + String(AppConfig::GEO_SENSOR_KEY) + "\r\n";
    request += "Content-Length: " + String(payload.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;
    bool success = false;
    if (cellularSendRequest(request)) {
        if (!waitForCellularRecv(AppConfig::CELL_SOCKET_OP_TIMEOUT_MS)) {
            Serial.println("Timed out waiting for HTTP response over cellular");
        }
        String httpResponse;
        if (readCellularHttpResponse(httpResponse)) {
            int statusCode = parseHttpStatusCode(httpResponse);
            Serial.printf("Cellular geoSensor HTTP status: %d\n", statusCode);
            success = statusCode >= 200 && statusCode < 300;
            if (!success) {
                Serial.println(httpResponse);
            }
        } else {
            Serial.println("Failed to read HTTP payload from modem");
        }
    }
    cellularCloseSocket();
    return success;
}

}  // namespace CellularClient

