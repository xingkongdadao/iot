
#include <HardwareSerial.h> // 引入硬件串口库以访问 UART 外设
#include <WiFi.h> // 引入官方 WiFi 库以管理 STA/AP/多模式连接
#include <WiFiClientSecure.h> // HTTPS 连接
#include <HTTPClient.h> // HTTP 请求封装
#include <stdio.h>
#include <math.h>

#define simSerial                 Serial0 // 将主控与 4G/GPS 模块之间的串口实例命名为 simSerial
#define MCU_SIM_BAUDRATE          115200 // 模块串口通信波特率
#define MCU_SIM_TX_PIN            21 // ESP32-C3 对应模块 RX 的 TX 引脚
#define MCU_SIM_RX_PIN            20 // ESP32-C3 对应模块 TX 的 RX 引脚
#define MCU_SIM_EN_PIN            2 // 模块电源/使能管脚；硬件 V2 需要用 IO2
#define MCU_LED                   10 // 板载 LED 管脚
#define PHONE_NUMBER            "0..." // 发送短信的目标手机号
#define GEO_SENSOR_UPLOAD_INTERVAL_MS 300000UL // geoSensor 上传周期（5 分钟）

// const char* GEO_SENSOR_API_BASE_URL = "http://192.168.100.192:8001/api"; // 本地测试地址，与 ESP32_2.ino 一致
const char* GEO_SENSOR_API_BASE_URL = "https://manage.gogotrans.com/api"; // 生产环境地址
const char* GEO_SENSOR_KEY = "mcu_0fda5a6b27214e1eb30fe7fe2c5d4f69"; // 接口 key
const char* GEO_SENSOR_ID = "4ccd94bc-c947-11f0-9ea2-12d3851b737f"; // GPS 传感器 ID
// Thay bằng thông tin WiFi của bạn
const char* ssid = "GOGOTRANS";        // 目标 Wi-Fi SSID
const char* password = "18621260183"; // 目标 Wi-Fi 密码
const uint8_t WIFI_MAX_ATTEMPTS = 5;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

struct GpsFix;

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
const size_t GEO_SENSOR_BUFFER_CAPACITY = 32;
GpsFix geoSensorBuffer[GEO_SENSOR_BUFFER_CAPACITY];
size_t geoSensorBufferStart = 0;
size_t geoSensorBufferCount = 0;

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
    geoSensorBufferStart = geoSensorBufferIndex(1);
    --geoSensorBufferCount;
}

void geoSensorBufferEnqueue(const GpsFix& fix) {
    if (geoSensorBufferCount == GEO_SENSOR_BUFFER_CAPACITY) {
        geoSensorBufferDropOldest();
    }
    size_t insertIndex = geoSensorBufferIndex(geoSensorBufferCount);
    geoSensorBuffer[insertIndex] = fix;
    ++geoSensorBufferCount;
    Serial.printf("Buffered geoSensor fix, count=%u\n", static_cast<unsigned>(geoSensorBufferCount));
}

bool geoSensorBufferPeek(GpsFix& fix) {
    if (geoSensorBufferEmpty()) {
        return false;
    }
    fix = geoSensorBuffer[geoSensorBufferStart];
    return true;
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

bool uploadGeoSensor(const GpsFix& fix) {
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

bool flushGeoSensorBuffer() {
    if (WiFi.status() != WL_CONNECTED) {
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
            break;
        }
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
    bool wifiReady = WiFi.status() == WL_CONNECTED;
    if (!wifiReady) {
        Serial.println("WiFi unavailable, buffering fix");
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
        geoSensorBufferEnqueue(fix);
    } else {
        Serial.println("geoSensor upload success");
    }
}

void loop() {
    if (Serial.available()) {
        simSerial.write(Serial.read());
    }
    bool wifiReady = ensureWifiConnected();
    if (wifiReady) {
        flushGeoSensorBuffer();
    }
    handleGeoSensorUpdate();
}
