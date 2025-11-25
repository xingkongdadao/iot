
#include <HardwareSerial.h> // 引入硬件串口库以访问 UART 外设
#include <WiFi.h> // 引入官方 WiFi 库以管理 STA/AP/多模式连接
#include <WiFiClientSecure.h> // HTTPS 连接
#include <HTTPClient.h> // HTTP 请求封装
#include <Preferences.h> // NVS 永久化存储
#include <stdio.h>
#include <math.h>
#include <string.h>

#define simSerial                 Serial0 // 将主控与 4G/GPS 模块之间的串口实例命名为 simSerial
#define MCU_SIM_BAUDRATE          115200 // 模块串口通信波特率
#define MCU_SIM_TX_PIN            21 // ESP32-C3 对应模块 RX 的 TX 引脚
#define MCU_SIM_RX_PIN            20 // ESP32-C3 对应模块 TX 的 RX 引脚
#define MCU_SIM_EN_PIN            2 // 模块电源/使能管脚；硬件 V2 需要用 IO2
#define MCU_LED                   10 // 板载 LED 管脚
#define PHONE_NUMBER            "0..." // 发送短信的目标手机号
#define GEO_SENSOR_UPLOAD_INTERVAL_MS 6000UL // geoSensor 上传周期（5 分钟）

const char* GEO_SENSOR_API_BASE_URL = "http://192.168.100.192:8001/api"; // 本地测试地址，与 ESP32_2.ino 一致
// const char* GEO_SENSOR_API_BASE_URL = "https://manage.gogotrans.com/api"; // 生产环境地址
const char* GEO_SENSOR_KEY = "mcu_0fda5a6b27214e1eb30fe7fe2c5d4f69"; // 接口 key
const char* GEO_SENSOR_ID = "4ccd94bc-c947-11f0-9ea2-12d3851b737f"; // GPS 传感器 ID
// Thay bằng thông tin WiFi của bạn
const char* ssid = "GOGOTRANS";        // 目标 Wi-Fi SSID
const char* password = "18621260183"; // 目标 Wi-Fi 密码
const uint8_t WIFI_MAX_ATTEMPTS = 5;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
const uint32_t GEO_SENSOR_BACKOFF_DELAYS_MS[] = {5000UL, 60000UL, 300000UL}; // 5 秒 -> 1 分钟 -> 5 分钟
const size_t GEO_SENSOR_BACKOFF_STAGE_COUNT =
    sizeof(GEO_SENSOR_BACKOFF_DELAYS_MS) / sizeof(GEO_SENSOR_BACKOFF_DELAYS_MS[0]);
const char* CELL_APN = "CMNET"; // 根据 SIM 卡运营商调整
const char* CELL_APN_USER = ""; // 若无需鉴权则留空
const char* CELL_APN_PASS = "";
const uint8_t CELL_CONTEXT_ID = 1;
const uint8_t CELL_SOCKET_ID = 0;
const uint32_t CELL_ATTACH_TIMEOUT_MS = 60000;
const uint32_t CELL_READY_REFRESH_MS = 300000;
const uint32_t CELL_SOCKET_OP_TIMEOUT_MS = 20000;
const uint16_t CELL_HTTP_READ_CHUNK = 512;

struct GpsFix;
struct ParsedUrl;

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

bool ensureWifiConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    for (uint8_t attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; ++attempt) {
        Serial.printf("WiFi connect attempt %u/%u\n", attempt, WIFI_MAX_ATTEMPTS);
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.begin(ssid, password);
        wl_status_t result = static_cast<wl_status_t>(WiFi.waitForConnectResult(WIFI_CONNECT_TIMEOUT_MS));
        if (result == WL_CONNECTED) {
            Serial.println("WiFi connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("RSSI: ");
            Serial.println(WiFi.RSSI());
            Serial.print("MAC Address: ");
            Serial.println(WiFi.macAddress());
            return true;
        }
        Serial.printf("WiFi connect failed -> status: %s (%d)\n", wifiStatusToString(result), result);
        delay(1000);
    }
    Serial.println("WiFi connection timeout after max attempts");
    return false;
}

struct GpsFix {
    float latitude = 0.0f;
    float longitude = 0.0f;
    float altitude = 0.0f;
    float speed = 0.0f;
    String dataAcquiredAt;
};

unsigned long lastGeoSensorPush = 0;
WiFiClientSecure geoSecureClient;
const size_t GEO_SENSOR_BUFFER_CAPACITY = 10000;
GpsFix geoSensorBuffer[GEO_SENSOR_BUFFER_CAPACITY];
size_t geoSensorBufferStart = 0;
size_t geoSensorBufferCount = 0;
Preferences geoPrefs;
bool geoPrefsReady = false;
const char* GEO_BUFFER_PREF_NAMESPACE = "geoBuf";
const char* GEO_BUFFER_PREF_START_KEY = "start";
const char* GEO_BUFFER_PREF_COUNT_KEY = "count";
int geoSensorBackoffStage = -1;
unsigned long geoSensorNextRetryAt = 0;
bool cellularContextReady = false;
bool cellularSocketOpen = false;
unsigned long lastCellularReadyCheck = 0;

String geoBufferSlotKey(size_t index) {
    return String("fix") + String(static_cast<unsigned>(index));
}

String serializeGpsFix(const GpsFix& fix) {
    String data;
    data.reserve(96);
    data += String(fix.latitude, 8);
    data += ",";
    data += String(fix.longitude, 8);
    data += ",";
    data += String(fix.altitude, 2);
    data += ",";
    data += String(fix.speed, 2);
    data += ",";
    data += fix.dataAcquiredAt;
    return data;
}

bool deserializeGpsFix(const String& data, GpsFix& fix) {
    const size_t FIELD_COUNT = 5;
    String fields[FIELD_COUNT];
    if (!splitCsvFields(data, fields, FIELD_COUNT)) {
        return false;
    }
    fix.latitude = fields[0].toFloat();
    fix.longitude = fields[1].toFloat();
    fix.altitude = fields[2].toFloat();
    fix.speed = fields[3].toFloat();
    fix.dataAcquiredAt = fields[4];
    return true;
}

void persistGeoSensorMetadata() {
    if (!geoPrefsReady) {
        return;
    }
    geoPrefs.putUChar(GEO_BUFFER_PREF_START_KEY, static_cast<uint8_t>(geoSensorBufferStart));
    geoPrefs.putUChar(GEO_BUFFER_PREF_COUNT_KEY, static_cast<uint8_t>(geoSensorBufferCount));
}

void persistGeoSensorSlot(size_t index) {
    if (!geoPrefsReady) {
        return;
    }
    String key = geoBufferSlotKey(index);
    geoPrefs.putString(key.c_str(), serializeGpsFix(geoSensorBuffer[index]));
}

void clearGeoSensorSlot(size_t index) {
    if (!geoPrefsReady) {
        return;
    }
    String key = geoBufferSlotKey(index);
    geoPrefs.remove(key.c_str());
}

void initGeoSensorBufferPersistence() {
    if (geoPrefsReady) {
        return;
    }
    if (!geoPrefs.begin(GEO_BUFFER_PREF_NAMESPACE, false)) {
        Serial.println("Failed to init geo buffer prefs, using RAM-only buffer");
        return;
    }
    geoPrefsReady = true;
    size_t storedStart = geoPrefs.getUChar(GEO_BUFFER_PREF_START_KEY, 0);
    size_t storedCount = geoPrefs.getUChar(GEO_BUFFER_PREF_COUNT_KEY, 0);
    if (storedStart >= GEO_SENSOR_BUFFER_CAPACITY) {
        storedStart = 0;
    }
    if (storedCount > GEO_SENSOR_BUFFER_CAPACITY) {
        storedCount = 0;
    }
    geoSensorBufferStart = storedStart;
    geoSensorBufferCount = 0;
    bool truncated = false;
    for (size_t offset = 0; offset < storedCount; ++offset) {
        size_t index = (storedStart + offset) % GEO_SENSOR_BUFFER_CAPACITY;
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
            size_t index = (storedStart + offset) % GEO_SENSOR_BUFFER_CAPACITY;
            clearGeoSensorSlot(index);
        }
    }
    persistGeoSensorMetadata();
    Serial.printf("Restored %u buffered fixes from flash\n", static_cast<unsigned>(geoSensorBufferCount));
}

bool geoSensorBufferEmpty() {
    return geoSensorBufferCount == 0;
}

size_t geoSensorBufferIndex(size_t offset) {
    return (geoSensorBufferStart + offset) % GEO_SENSOR_BUFFER_CAPACITY;
}

void geoSensorBufferDropOldest() {
    if (geoSensorBufferEmpty()) {
        return;
    }
    size_t dropIndex = geoSensorBufferStart;
    clearGeoSensorSlot(dropIndex);
    geoSensorBufferStart = geoSensorBufferIndex(1);
    --geoSensorBufferCount;
    persistGeoSensorMetadata();
}

void geoSensorBufferEnqueue(const GpsFix& fix) {
    if (geoSensorBufferCount == GEO_SENSOR_BUFFER_CAPACITY) {
        geoSensorBufferDropOldest();
    }
    size_t insertIndex = geoSensorBufferIndex(geoSensorBufferCount);
    geoSensorBuffer[insertIndex] = fix;
    ++geoSensorBufferCount;
    persistGeoSensorSlot(insertIndex);
    persistGeoSensorMetadata();
    Serial.printf("Buffered geoSensor fix, count=%u\n", static_cast<unsigned>(geoSensorBufferCount));
}

bool geoSensorBufferPeek(GpsFix& fix) {
    if (geoSensorBufferEmpty()) {
        return false;
    }
    fix = geoSensorBuffer[geoSensorBufferStart];
    return true;
}

bool geoSensorUploadReady() {
    return geoSensorBackoffStage < 0 || millis() >= geoSensorNextRetryAt;
}

void geoSensorRecordUploadFailure() {
    if (geoSensorBackoffStage < 0) {
        geoSensorBackoffStage = 0;
    } else if (geoSensorBackoffStage < static_cast<int>(GEO_SENSOR_BACKOFF_STAGE_COUNT) - 1) {
        ++geoSensorBackoffStage;
    }
    unsigned long delayMs = GEO_SENSOR_BACKOFF_DELAYS_MS[geoSensorBackoffStage];
    geoSensorNextRetryAt = millis() + delayMs;
    Serial.printf("geoSensor upload failed, will retry after %lu ms\n", delayMs);
}

void geoSensorRecordUploadSuccess() {
    geoSensorBackoffStage = -1;
    geoSensorNextRetryAt = 0;
}


void sim_at_wait() {
    delay(500); // 留出时间等待模块返回
    while (simSerial.available()) { // 只要模块缓冲区有数据
        Serial.write(simSerial.read()); // 逐字节转发到 USB 串口，方便调试
    }
}


bool sim_at_cmd(String cmd) {
    Serial.print("Sending command: "); // 控制台提示
    Serial.println(cmd); // 打印即将发送的 AT 指令
    simSerial.println(cmd); // 将完整指令写入模块串口（带换行）
    delay(500); // 简单等待响应
    sim_at_wait(); // 读取并显示模块回复
    return true; // 简单返回 true，未做超时判断
}

bool sim_at_send(char c) {
    simSerial.write(c); // 直接写入一个字节（多用于发送 Ctrl+Z）
    return true; // 同样固定返回 true
}

void sent_sms() {
    sim_at_cmd("AT+CMGF=1"); // 设置短信文本模式
    String temp = "AT+CMGS=\""; // 组装发送号码指令头
    temp += PHONE_NUMBER; // 填入目标号码
    temp += "\""; // 补齐结尾引号
    sim_at_cmd(temp); // 发送号码指令
    sim_at_cmd("hello from TDLOGY"); // 写入短信内容
    sim_at_send(0x1A); // 发送 Ctrl+Z 告知模块完成输入
}



void get_gps_data() {
    // Bật GPS
    sim_at_cmd("AT+QGPS=1"); // 打开 GNSS 引擎
    delay(60000); // 延长等待到 60 秒，增加首定位成功率
    sim_at_cmd("AT+QGPS?"); // 查询 GPS 状态
    sim_at_cmd("AT+QGPSLOC=0"); // Nhận thông tin GPS
}

void setup() {
    pinMode(MCU_SIM_EN_PIN, OUTPUT); // 设置模块电源引脚为输出
    digitalWrite(MCU_SIM_EN_PIN, HIGH); // 拉高以打开模块
    delay(500);   // Thả PWRKEY lên cao
    Serial.begin(115200); // 初始化 USB 串口用于调试
    pinMode(MCU_LED, OUTPUT); // LED 引脚输出模式
    digitalWrite(MCU_LED, HIGH); // 点亮 LED 提示系统启动
    Serial.println("\n\n\n\n-----------------------\nSystem started!!!!"); // 打印启动提示
    delay(8000); // 留出模块上电稳定时间
    initGeoSensorBufferPersistence(); // 在尝试联网前恢复历史缓存
    Serial.println("Starting ESP32 WiFi Test..."); // 提示即将进行 Wi-Fi 测试
    Serial.print("Connecting to "); // 打印连接中的 SSID
    Serial.println(ssid); // 显示目标网络
    if (!ensureWifiConnected()) {
        Serial.println("WiFi unavailable, will retry in loop");
    }

    simSerial.begin(MCU_SIM_BAUDRATE, SERIAL_8N1, MCU_SIM_RX_PIN, MCU_SIM_TX_PIN); // 初始化模块串口
    Serial.println("Checking AT command..."); // 提示开始检测 AT
    sim_at_cmd("AT"); // 发送基本 AT 测试
    Serial.println("Getting product info..."); // 提示查询模块信息
    sim_at_cmd("ATI"); // 读取模块版本/型号
    Serial.println("Checking SIM status..."); // 提示检测 SIM
    sim_at_cmd("AT+CPIN?"); // 查询 SIM 卡状态
    Serial.println("Checking signal quality..."); // 提示检测信号
    sim_at_cmd("AT+CSQ"); // 查询信号强度
    Serial.println("Getting IMSI..."); // 提示读取 IMSI
    sim_at_cmd("AT+CIMI"); // 读取 IMSI
    //sent_sms();
    // delay(5000);
    ensureWifiConnected();
    get_gps_data(); // 执行一次 GPS 初始化与定位
    geoSecureClient.setInsecure(); // 跳过证书校验（仅用于调试环境）

}
bool sim_at_cmd_with_response(const String& cmd, String& response, uint32_t timeoutMs = 5000) {
    Serial.print("Sending command: ");
    Serial.println(cmd);
    simSerial.println(cmd);
    response = "";
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (simSerial.available()) {
            char c = simSerial.read();
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

bool waitForSubstring(const String& expect, uint32_t timeoutMs, String* response = nullptr) {
    unsigned long start = millis();
    String buffer;
    while (millis() - start < timeoutMs) {
        while (simSerial.available()) {
            char c = simSerial.read();
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
                       uint32_t timeoutMs = 5000,
                       String* response = nullptr) {
    Serial.print("Sending command: ");
    Serial.println(cmd);
    simSerial.println(cmd);
    return waitForSubstring(expect, timeoutMs, response);
}

bool splitCsvFields(const String& data, String* outFields, size_t expectedCount) {
    size_t idx = 0;
    int start = 0;
    while (idx < expectedCount && start <= data.length()) {
        int next = data.indexOf(',', start);
        if (next == -1) {
            next = data.length();
        }
        outFields[idx++] = data.substring(start, next);
        start = next + 1;
    }
    return idx == expectedCount;
}

float convertNmeaToDecimal(const String& raw) {
    if (raw.length() < 2) {
        return 0.0f;
    }
    char hemi = raw.charAt(raw.length() - 1);
    String numeric = raw.substring(0, raw.length() - 1);
    float value = numeric.toFloat();
    int degrees = static_cast<int>(value / 100.0f);
    float minutes = value - degrees * 100.0f;
    float decimal = degrees + minutes / 60.0f;
    if (hemi == 'S' || hemi == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

String buildIso8601UtcFromGps(const String& dateField, const String& timeField) {
    if (dateField.length() < 6 || timeField.length() < 6) {
        return "";
    }
    int day = dateField.substring(0, 2).toInt();
    int month = dateField.substring(2, 4).toInt();
    int year = 2000 + dateField.substring(4, 6).toInt();
    int hour = timeField.substring(0, 2).toInt();
    int minute = timeField.substring(2, 4).toInt();
    int second = timeField.substring(4, 6).toInt();
    char buffer[32];
    snprintf(buffer,
             sizeof(buffer),
             "%04d-%02d-%02dT%02d:%02d:%02d+00:00",
             year,
             month,
             day,
             hour,
             minute,
             second);
    return String(buffer);
}

bool parseGpsResponse(const String& raw, GpsFix& fix) {
    int tagPos = raw.indexOf("+QGPSLOC:");
    if (tagPos == -1) {
        return false;
    }
    int lineEnd = raw.indexOf("\n", tagPos);
    if (lineEnd == -1) {
        lineEnd = raw.length();
    }
    String data = raw.substring(tagPos + 10, lineEnd);
    data.trim();
    data.replace("\r", "");
    const size_t FIELD_COUNT = 11;
    String fields[FIELD_COUNT];
    if (!splitCsvFields(data, fields, FIELD_COUNT)) {
        return false;
    }
    fix.latitude = convertNmeaToDecimal(fields[1]);
    fix.longitude = convertNmeaToDecimal(fields[2]);
    fix.altitude = fields[4].toFloat();
    fix.speed = fields[7].toFloat();
    fix.dataAcquiredAt = buildIso8601UtcFromGps(fields[9], fields[0]);
    return true;
}

bool fetchGpsFix(GpsFix& fix) {
    String response;
    if (!sim_at_cmd_with_response("AT+QGPSLOC=0", response)) {
        return false;
    }
    return parseGpsResponse(response, fix);
}

String formatCoordinate(double value) {
    double absVal = fabs(value);
    int digitsBeforeDecimal = 1;
    if (absVal >= 1.0) {
        digitsBeforeDecimal = (int)floor(log10(absVal)) + 1;
    }
    int decimals = 8 - digitsBeforeDecimal;
    if (decimals < 0) {
        decimals = 0;
    } else if (decimals > 6) {
        decimals = 6;
    }
    return String(value, decimals);
}

String buildGeoSensorPayload(const GpsFix& fix) {
    String payload = "{";
    payload += "\"sensorId\":\"" + String(GEO_SENSOR_ID) + "\",";
    payload += "\"latitude\":" + formatCoordinate(fix.latitude) + ",";
    payload += "\"longitude\":" + formatCoordinate(fix.longitude) + ",";
    payload += "\"altitude\":" + String(fix.altitude, 2) + ",";
    payload += "\"speed\":" + String(fix.speed, 2) + ",";
    payload += "\"dataAcquiredAt\":\"" + fix.dataAcquiredAt + "\"";
    payload += "}";
    return payload;
}

struct ParsedUrl {
    String host;
    String path = "/";
    uint16_t port = 80;
    bool https = false;
};

bool parseUrl(const String& url, ParsedUrl& parsed) {
    int schemeSep = url.indexOf("://");
    if (schemeSep == -1) {
        return false;
    }
    String scheme = url.substring(0, schemeSep);
    parsed.https = scheme.equalsIgnoreCase("https");
    int hostStart = schemeSep + 3;
    int pathStart = url.indexOf('/', hostStart);
    String hostPort = pathStart == -1 ? url.substring(hostStart) : url.substring(hostStart, pathStart);
    if (hostPort.length() == 0) {
        return false;
    }
    int colon = hostPort.indexOf(':');
    if (colon == -1) {
        parsed.host = hostPort;
        parsed.port = parsed.https ? 443 : 80;
    } else {
        parsed.host = hostPort.substring(0, colon);
        parsed.port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
        if (parsed.port == 0) {
            parsed.port = parsed.https ? 443 : 80;
        }
    }
    if (pathStart == -1) {
        parsed.path = "/";
    } else {
        parsed.path = url.substring(pathStart);
        if (parsed.path.length() == 0) {
            parsed.path = "/";
        }
    }
    return true;
}

bool qiactResponseHasContext(const String& response) {
    String needle = "+QIACT: " + String(CELL_CONTEXT_ID) + ",";
    return response.indexOf(needle) != -1;
}

bool ensureCellularReady() {
    if (CELL_APN == nullptr || strlen(CELL_APN) == 0) {
        return false;
    }
    if (cellularContextReady && millis() - lastCellularReadyCheck < CELL_READY_REFRESH_MS) {
        return true;
    }
    String response;
    if (!sim_at_cmd_with_response("AT", response, 2000)) {
        Serial.println("Cellular module not responding to AT");
        return false;
    }
    String apnCmd = "AT+QICSGP=" + String(CELL_CONTEXT_ID) + ",1,\"" + String(CELL_APN) + "\",\"" +
                    String(CELL_APN_USER) + "\",\"" + String(CELL_APN_PASS) + "\",1";
    if (!sim_at_cmd_with_response(apnCmd, response, 5000)) {
        Serial.println("Failed to configure APN");
        return false;
    }
    if (!sim_at_cmd_with_response("AT+QIACT?", response, 5000) || !qiactResponseHasContext(response)) {
        String actCmd = "AT+QIACT=" + String(CELL_CONTEXT_ID);
        if (!sim_at_cmd_with_response(actCmd, response, CELL_ATTACH_TIMEOUT_MS)) {
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

void cellularCloseSocket() {
    if (!cellularSocketOpen) {
        return;
    }
    String closeCmd = "AT+QICLOSE=" + String(CELL_SOCKET_ID);
    String response;
    sim_at_cmd_with_response(closeCmd, response, 5000);
    waitForSubstring("+QIURC: \"closed\"," + String(CELL_SOCKET_ID), 2000, nullptr);
    cellularSocketOpen = false;
}

bool cellularOpenSocket(const ParsedUrl& parsed) {
    cellularCloseSocket();
    String openCmd = "AT+QIOPEN=" + String(CELL_CONTEXT_ID) + "," + String(CELL_SOCKET_ID) + ",\"TCP\",\"" +
                     parsed.host + "\"," + String(parsed.port) + ",0,1";
    String response;
    if (!sim_at_cmd_with_response(openCmd, response, CELL_SOCKET_OP_TIMEOUT_MS)) {
        Serial.println("AT+QIOPEN command failed");
        return false;
    }
    if (!waitForSubstring("+QIOPEN: " + String(CELL_SOCKET_ID) + ",0", CELL_SOCKET_OP_TIMEOUT_MS, nullptr)) {
        Serial.println("Socket open URC not received");
        return false;
    }
    cellularSocketOpen = true;
    return true;
}

bool cellularSendRequest(const String& request) {
    if (!sim_at_cmd_expect("AT+QISEND=" + String(CELL_SOCKET_ID), ">", 5000, nullptr)) {
        Serial.println("QISEND prompt not received");
        return false;
    }
    simSerial.print(request);
    simSerial.write(0x1A);
    if (!waitForSubstring("SEND OK", CELL_SOCKET_OP_TIMEOUT_MS, nullptr)) {
        Serial.println("SEND OK not received");
        return false;
    }
    return true;
}

bool waitForCellularRecv(uint32_t timeoutMs) {
    String marker = "+QIURC: \"recv\"," + String(CELL_SOCKET_ID);
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
        String cmd = "AT+QIRD=" + String(CELL_SOCKET_ID) + "," + String(CELL_HTTP_READ_CHUNK);
        if (!sim_at_cmd_with_response(cmd, chunk, CELL_SOCKET_OP_TIMEOUT_MS)) {
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
        if (reportedLen < CELL_HTTP_READ_CHUNK) {
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

bool uploadGeoSensorViaCellular(const GpsFix& fix) {
    if (CELL_APN == nullptr || strlen(CELL_APN) == 0) {
        Serial.println("CELL_APN not configured, skip cellular upload");
        return false;
    }
    String fullUrl = String(GEO_SENSOR_API_BASE_URL) + "/device/geoSensor/" + String(GEO_SENSOR_ID) + "/";
    ParsedUrl parsed;
    if (!parseUrl(fullUrl, parsed)) {
        Serial.println("Failed to parse geoSensor URL");
        return false;
    }
    if (parsed.https) {
        Serial.println("Cellular fallback currently supports HTTP only");
        return false;
    }
    if (!ensureCellularReady()) {
        return false;
    }
    if (!cellularOpenSocket(parsed)) {
        return false;
    }
    String payload = buildGeoSensorPayload(fix);
    String hostHeader = parsed.host;
    if (parsed.port != 80 && parsed.port != 443) {
        hostHeader += ":" + String(parsed.port);
    }
    String request = "PATCH " + parsed.path + " HTTP/1.1\r\n";
    request += "Host: " + hostHeader + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "X-API-Key: " + String(GEO_SENSOR_KEY) + "\r\n";
    request += "Content-Length: " + String(payload.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;
    bool success = false;
    if (cellularSendRequest(request)) {
        if (!waitForCellularRecv(CELL_SOCKET_OP_TIMEOUT_MS)) {
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

bool uploadGeoSensorViaWifi(const GpsFix& fix) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, abort geoSensor upload");
        return false;
    }

    HTTPClient http;
    http.setTimeout(10000);

    String url = String(GEO_SENSOR_API_BASE_URL) + "/device/geoSensor/" + String(GEO_SENSOR_ID) + "/";
    String payload = buildGeoSensorPayload(fix);
    bool useHttps = url.startsWith("https://");
    bool beginResult = false;

    if (useHttps) {
        geoSecureClient.stop(); // 确保使用全新 TLS 连接
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
    http.addHeader("X-API-Key", GEO_SENSOR_KEY);
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

bool uploadGeoSensor(const GpsFix& fix) {
    if (WiFi.status() == WL_CONNECTED) {
        if (uploadGeoSensorViaWifi(fix)) {
            return true;
        }
        Serial.println("WiFi upload failed, trying cellular fallback");
    }
    return uploadGeoSensorViaCellular(fix);
}

bool flushGeoSensorBuffer() {
    if (!geoSensorUploadReady()) {
        return false;
    }
    bool uploadedAny = false;
    while (!geoSensorBufferEmpty()) {
        GpsFix next;
        if (!geoSensorBufferPeek(next)) {
            break;
        }
        if (!uploadGeoSensor(next)) {
            Serial.println("Buffered geoSensor upload failed, will retry later");
            geoSensorRecordUploadFailure();
            break;
        }
        geoSensorRecordUploadSuccess();
        geoSensorBufferDropOldest();
        uploadedAny = true;
        Serial.printf("Buffered geoSensor upload success, remaining=%u\n",
                      static_cast<unsigned>(geoSensorBufferCount));
    }
    return uploadedAny;
}

void handleGeoSensorUpdate() {
    unsigned long now = millis();
    if (lastGeoSensorPush != 0 && now - lastGeoSensorPush < GEO_SENSOR_UPLOAD_INTERVAL_MS) {
        return;
    }
    lastGeoSensorPush = now;
    Serial.println("handleGeoSensorUpdate triggered");
    GpsFix fix;
    if (!fetchGpsFix(fix)) {
        Serial.println("Failed to acquire GPS fix");
        return;
    }
    if (!geoSensorUploadReady()) {
        Serial.println("geoSensor upload postponed by backoff, buffering");
        geoSensorBufferEnqueue(fix);
        return;
    }
    if (!geoSensorBufferEmpty()) {
        geoSensorBufferEnqueue(fix);
        flushGeoSensorBuffer();
        return;
    }
    if (!uploadGeoSensor(fix)) {
        Serial.println("Immediate geoSensor upload failed, buffering");
        geoSensorRecordUploadFailure();
        geoSensorBufferEnqueue(fix);
    } else {
        geoSensorRecordUploadSuccess();
        Serial.println("geoSensor upload success");
    }
}

void loop() {
    if (Serial.available()) {
        simSerial.write(Serial.read());
    }
    ensureWifiConnected();
    flushGeoSensorBuffer();
    handleGeoSensorUpdate();
}
