/*
 * ESP32 数据收集与上传程序（ ESP32S3-N16R8 ）
 * ----------------------------------------
 * 功能：
 * - 每5秒收集一条数据（模拟长度 + 日期时间含时区）
 * - 智能上传策略：
 *   * 如果本地没有待上传数据且网络正常，优先直接上传到后台（不保存到本地）
 *   * 如果本地有待上传数据或网络不可用，则保存到本地
 *   * 直接上传失败时，自动保存到本地等待后续上传
 * - 只要有网络且本地有数据，持续批量上传（不受5秒间隔限制）
 * - 严格 FIFO 顺序：先保存的数据先上传，批量上传成功后批量删除
 * - 批量上传：每次上传多条数据（默认50条），提高上传效率
 * - 可靠性保证：如果某条数据上传失败，停止本次批量上传，保留剩余数据等待下次重试
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>

// WiFi 配置（与 ESP32_1.ino 保持一致）
const char* defaultWifiSsid = "GOGOTRANS";
const char* defaultWifiPassword = "18621260183";

Preferences wifiPrefs;
String activeSsid = defaultWifiSsid;
String activePassword = defaultWifiPassword;
bool storedCredentialsAvailable = false;

constexpr char WIFI_PREF_NAMESPACE[] = "wifi";
constexpr char WIFI_PREF_SSID_KEY[] = "ssid";
constexpr char WIFI_PREF_PASS_KEY[] = "pass";

constexpr char CONFIG_AP_SSID[] = "ESP32S3_Config";
constexpr char CONFIG_AP_PASSWORD[] = "12345678";
constexpr unsigned long CONFIG_PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;  // 10分钟

WebServer configServer(80);
bool configPortalActive = false;
unsigned long configPortalStartTime = 0;
unsigned long configPortalLastActivity = 0;

void loadStoredWiFiCredentials() {
  storedCredentialsAvailable = false;
  if (wifiPrefs.begin(WIFI_PREF_NAMESPACE, true)) {
    String storedSsid = wifiPrefs.getString(WIFI_PREF_SSID_KEY, "");
    String storedPass = wifiPrefs.getString(WIFI_PREF_PASS_KEY, "");
    wifiPrefs.end();

    storedSsid.trim();
    storedPass.trim();

    if (storedSsid.length() > 0) {
      activeSsid = storedSsid;
      activePassword = storedPass;
      storedCredentialsAvailable = true;
      Serial.println("[WiFi] 已加载保存的 Wi-Fi 配置信息");
      return;
    }
  }

  activeSsid = defaultWifiSsid;
  activePassword = defaultWifiPassword;
  Serial.println("[WiFi] 使用默认的 Wi-Fi 配置信息");
}

bool persistWiFiCredentials(const String& newSsid, const String& newPassword) {
  if (!wifiPrefs.begin(WIFI_PREF_NAMESPACE, false)) {
    return false;
  }
  bool ok = wifiPrefs.putString(WIFI_PREF_SSID_KEY, newSsid) &&
            wifiPrefs.putString(WIFI_PREF_PASS_KEY, newPassword);
  wifiPrefs.end();
  if (ok) {
    activeSsid = newSsid;
    activePassword = newPassword;
    storedCredentialsAvailable = true;
  }
  return ok;
}

void sendConfigPortalPage(const String& message = "") {
  String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>ESP32 Wi-Fi 设置</title>"
      "<style>body{font-family:Arial;background:#f5f5f5;margin:0;padding:0;color:#333;}"
      ".container{max-width:420px;margin:40px auto;padding:24px;background:#fff;border-radius:8px;"
      "box-shadow:0 4px 12px rgba(0,0,0,0.08);}input,button{width:100%;padding:12px;margin:8px 0;"
      "border:1px solid #ccc;border-radius:6px;}button{background:#0070f3;color:#fff;border:none;"
      "font-size:16px;cursor:pointer;}button:hover{background:#005ad1;}label{font-weight:600;}"
      ".msg{margin-top:12px;color:#d9534f;font-weight:600;text-align:center;}"
      "</style></head><body><div class='container'><h2>配置 Wi-Fi</h2>"
      "<form method='POST' action='/save'><label>Wi-Fi 名称 (SSID)</label>"
      "<input name='ssid' placeholder='如：MyHomeWiFi' required>"
      "<label>Wi-Fi 密码</label><input name='password' type='password'"
      "placeholder='至少8位（如有）'>"
      "<button type='submit'>保存并重启</button></form>");
  if (message.length() > 0) {
    html += "<div class='msg'>" + message + "</div>";
  }
  html += F("<p style='font-size:12px;color:#777;margin-top:16px;'>保存后设备将自动重启，"
            "并尝试连接到新的 Wi-Fi。</p></div></body></html>");
  configServer.send(200, "text/html", html);
}

void handleConfigPortalRoot() {
  configPortalLastActivity = millis();
  sendConfigPortalPage();
}

void handleConfigPortalSave() {
  configPortalLastActivity = millis();
  if (!configServer.hasArg("ssid")) {
    sendConfigPortalPage("请填写 Wi-Fi 名称");
    return;
  }

  String newSsid = configServer.arg("ssid");
  String newPassword = configServer.arg("password");
  newSsid.trim();
  newPassword.trim();

  if (newSsid.isEmpty()) {
    sendConfigPortalPage("Wi-Fi 名称不能为空");
    return;
  }

  if (!persistWiFiCredentials(newSsid, newPassword)) {
    sendConfigPortalPage("保存失败，请重试");
    return;
  }

  configServer.send(200, "text/plain",
                    "Wi-Fi 配置已保存，设备即将重启并连接到新网络。");
  Serial.println("[WiFi] 已保存新的 Wi-Fi 配置，准备重启...");
  delay(1500);
  ESP.restart();
}

void handleConfigPortalNotFound() {
  configPortalLastActivity = millis();
  configServer.sendHeader("Location", "/", true);
  configServer.send(302, "text/plain", "");
}

void startConfigPortal() {
  if (configPortalActive) {
    return;
  }
  configPortalActive = true;
  configPortalStartTime = millis();
  configPortalLastActivity = configPortalStartTime;

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);

  bool apStarted = WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();

  configServer.on("/", HTTP_GET, handleConfigPortalRoot);
  configServer.on("/save", HTTP_POST, handleConfigPortalSave);
  configServer.onNotFound(handleConfigPortalNotFound);
  configServer.begin();

  Serial.println("\n[WiFi] 进入配置模式");
  if (apStarted) {
    Serial.print("[WiFi] 配置热点: ");
    Serial.print(CONFIG_AP_SSID);
    Serial.print(" / 密码: ");
    Serial.println(CONFIG_AP_PASSWORD);
    Serial.print("[WiFi] 使用手机连接后，访问 http://");
    Serial.print(apIP.toString());
    Serial.println(" 进行配置");
  } else {
    Serial.println("[WiFi] 启动配置热点失败");
  }
}

// API 配置（与 ESP32_1.ino 保持一致）
// 本地测试地址：http://192.168.100.193:8001/api
// 生产环境地址：https://manage.gogotrans.com/api
const char* apiBaseUrl = "http://192.168.100.192:8001/api";  // 本地测试地址
// const char* apiBaseUrl = "https://manage.gogotrans.com/api";  // 生产环境地址
const char* apiKey = "mcu_8312592b29fd4c68a0e01336cf26f438";
const char ultrasonicSensorId[] = "8ea58210-c649-11f0-afa3-da038af01e18";

// NTP 时间同步配置
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;  // 东七区（UTC+7），单位：秒
const int daylightOffset_sec = 0;     // 夏令时偏移
bool timeSynced = false;              // 时间是否已同步

// 数据收集间隔（毫秒）
const unsigned long collectInterval = 5000;  // 每5秒收集一条数据
unsigned long lastCollectTime = 0;

// 上传检查间隔（毫秒）- 持续检查并上传
const unsigned long uploadCheckInterval = 500;  // 每0.5秒检查一次是否有数据需要上传
unsigned long lastUploadCheckTime = 0;
static bool isUploading = false;  // 上传进行中标志，防止重复触发

// 批量上传配置
const int BATCH_UPLOAD_SIZE = 50;  // 每次批量上传的数据条数（Django后台支持，可根据需要调整：10-100条）

// 持久化存储配置（使用 LittleFS）
const char* dataFilePath = "/sensor_data.json";  // 数据文件路径
const size_t minFreeHeap = 20000;                // 最小可用堆内存阈值（字节）
const size_t JSON_DOC_SIZE = 1048576;            // JSON 文档大小（1MB），可存储约52400条数据

// 连接 WiFi
bool connectWiFi() {
  Serial.print("[WiFi] 正在连接: ");
  Serial.println(activeSsid);
  if (storedCredentialsAvailable) {
    Serial.println("[WiFi] 使用已保存的网络配置");
  }

  WiFi.begin(activeSsid.c_str(), activePassword.c_str());

  int attempts = 0;
  const int maxAttempts = 20;  // 最多尝试20次（10秒）
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    yield();
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ✓ 连接成功! IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] 信号强度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return true;
  } else {
    Serial.println("[WiFi] ✗ 连接失败，将在 loop 中继续尝试或进入配置模式");
    return false;
  }
}

// 同步 NTP 时间
void syncNTPTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[时间] WiFi 未连接，无法同步时间");
    return;
  }
  
  Serial.println("[时间] 正在同步 NTP 时间...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.nist.gov", "time.google.com");
  
  // 等待时间同步（最多等待10秒）
  int attempts = 0;
  time_t now = 0;
  while (attempts < 20) {
    delay(500);
    yield();
    now = time(nullptr);
    if (now > 1000000000) {
      break;
    }
    attempts++;
  }
  
  if (now > 1000000000) {
    timeSynced = true;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    Serial.print("[时间] ✓ 时间同步成功: ");
    Serial.print(timeinfo.tm_year + 1900);
    Serial.print("-");
    Serial.print(timeinfo.tm_mon + 1);
    Serial.print("-");
    Serial.print(timeinfo.tm_mday);
    Serial.print(" ");
    Serial.print(timeinfo.tm_hour);
    Serial.print(":");
    Serial.print(timeinfo.tm_min);
    Serial.print(":");
    Serial.println(timeinfo.tm_sec);
  } else {
    Serial.println("[时间] ✗ 时间同步失败");
    timeSynced = false;
  }
}

// 获取当前时间戳
time_t getCurrentTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000) {
    return 0;  // 时间未同步
  }
  return now;
}

// 格式化时间为 ISO 8601 格式字符串（YYYY-MM-DDTHH:MM:SS+07:00，包含时区）
String formatDateTime(time_t timestamp) {
  if (timestamp == 0) {
    return "";  // 时间未同步时返回空字符串
  }
  
  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);
  
  // 计算时区偏移（小时和分钟）
  int offsetHours = gmtOffset_sec / 3600;
  int offsetMinutes = abs((gmtOffset_sec % 3600) / 60);
  
  // 格式化时区字符串（+07:00 或 -05:00）
  char timezoneStr[7];
  if (offsetHours >= 0) {
    snprintf(timezoneStr, sizeof(timezoneStr), "+%02d:%02d", offsetHours, offsetMinutes);
  } else {
    snprintf(timezoneStr, sizeof(timezoneStr), "%03d:%02d", offsetHours, offsetMinutes);
  }
  
  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%s",
           timeinfo.tm_year + 1900,
           timeinfo.tm_mon + 1,
           timeinfo.tm_mday,
           timeinfo.tm_hour,
           timeinfo.tm_min,
           timeinfo.tm_sec,
           timezoneStr);
  
  return String(buffer);
}

// 保存数据到持久化存储（LittleFS）
bool saveDataToStorage(float distanceCm, time_t timestamp) {
  // 读取现有数据
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  
  int storedCount = 0;
  
  if (LittleFS.exists(dataFilePath)) {
    File file = LittleFS.open(dataFilePath, "r");
    if (file) {
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      if (!error && doc.containsKey("a")) {
        // 使用实际数组大小，而不是计数器（确保准确性）
        JsonArray tempArray = doc["a"].as<JsonArray>();
        storedCount = tempArray.size();
      }
    }
  }
  
  // 如果文件不存在或解析失败，初始化 JSON 结构
  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.isNull()) {
    doc.remove("a");
    dataArray = doc.createNestedArray("a");
    doc["c"] = 0;
    storedCount = 0;
  }
  
  // 检查可用堆内存，如果低于阈值则删除最旧的数据（FIFO）
  size_t freeHeap = ESP.getFreeHeap();
  int deletedCount = 0;
  
  while (freeHeap < minFreeHeap && dataArray.size() > 0) {
    if (deletedCount == 0) {
      Serial.print("[警告] 可用内存不足 (");
      Serial.print(freeHeap);
      Serial.print(" 字节 < ");
      Serial.print(minFreeHeap);
      Serial.println(" 字节)，开始删除最旧的数据");
    }
    
    // 删除最旧的数据（数组第一个元素）
    dataArray.remove(0);
    storedCount--;
    deletedCount++;
    
    // 再次检查内存
    freeHeap = ESP.getFreeHeap();
  }
  
  if (deletedCount > 0) {
    Serial.print("[信息] 已删除 ");
    Serial.print(deletedCount);
    Serial.print(" 条最旧数据，当前可用内存: ");
    Serial.print(freeHeap);
    Serial.print(" 字节，剩余数据: ");
    Serial.print(storedCount);
    Serial.println(" 条");
  }
  
  // 添加新数据（使用数组格式以节省空间：[distance, timestamp]）
  JsonArray newRecord = dataArray.createNestedArray();
  newRecord.add(distanceCm);
  newRecord.add((uint64_t)timestamp);
  // 使用实际数组大小更新计数器（确保准确性）
  doc["c"] = dataArray.size();
  
  // 保存到文件
  File file = LittleFS.open(dataFilePath, "w");
  if (!file) {
    Serial.println("[错误] 无法打开文件进行写入");
    return false;
  }
  
  serializeJson(doc, file);
  file.close();
  
  return true;
}

// 获取持久化存储中的数据条数（返回实际数组大小，确保准确性）
int getStoredDataCount() {
  if (!LittleFS.exists(dataFilePath)) {
    return 0;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return 0;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return 0;
  }
  
  // 返回实际数组大小，而不是计数器（确保准确性）
  if (doc.containsKey("a")) {
    JsonArray dataArray = doc["a"].as<JsonArray>();
    return dataArray.size();
  }
  
  return 0;
}

// 从持久化存储读取第一条数据（最早的数据）
bool readFirstDataFromStorage(float& distance, time_t& timestamp) {
  if (!LittleFS.exists(dataFilePath)) {
    return false;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return false;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return false;
  }
  
  if (!doc.containsKey("a")) {
    return false;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.size() == 0) {
    return false;
  }
  
  // 读取第一条数据
  JsonArray record = dataArray[0];
  if (record.size() >= 2) {
    distance = record[0].as<float>();
    timestamp = (time_t)record[1].as<uint64_t>();
    return true;
  }
  
  return false;
}

// 从持久化存储删除第一条数据
void removeFirstDataFromStorage() {
  if (!LittleFS.exists(dataFilePath)) {
    return;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return;
  }
  
  // 检查是否有数据
  if (!doc.containsKey("a")) {
    return;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.size() == 0) {
    return;
  }
  
  // 删除第一条数据
  dataArray.remove(0);
  // 使用实际数组大小更新计数器（确保准确性）
  doc["c"] = dataArray.size();
  
  // 保存回文件
  file = LittleFS.open(dataFilePath, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

// 从持久化存储批量读取前N条数据（FIFO顺序）
// 返回实际读取的数据条数
int readBatchDataFromStorage(float* distances, time_t* timestamps, int maxCount) {
  if (!LittleFS.exists(dataFilePath)) {
    return 0;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return 0;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return 0;
  }
  
  if (!doc.containsKey("a")) {
    return 0;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  int arraySize = dataArray.size();
  if (arraySize == 0) {
    return 0;
  }
  
  // 读取前N条数据（不超过maxCount和实际数组大小）
  int readCount = (arraySize < maxCount) ? arraySize : maxCount;
  
  for (int i = 0; i < readCount; i++) {
    JsonArray record = dataArray[i];
    if (record.size() >= 2) {
      distances[i] = record[0].as<float>();
      timestamps[i] = (time_t)record[1].as<uint64_t>();
    } else {
      // 数据格式错误，返回已读取的数量
      return i;
    }
  }
  
  return readCount;
}

// 从持久化存储批量删除前N条数据（FIFO顺序）
void removeBatchDataFromStorage(int count) {
  if (!LittleFS.exists(dataFilePath) || count <= 0) {
    return;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return;
  }
  
  // 检查是否有数据
  if (!doc.containsKey("a")) {
    return;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  int arraySize = dataArray.size();
  if (arraySize == 0) {
    return;
  }
  
  // 删除前N条数据（不超过实际数组大小）
  int deleteCount = (arraySize < count) ? arraySize : count;
  
  for (int i = 0; i < deleteCount; i++) {
    dataArray.remove(0);
  }
  
  // 使用实际数组大小更新计数器（确保准确性）
  doc["c"] = dataArray.size();
  
  // 保存回文件
  file = LittleFS.open(dataFilePath, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

// 上传单条数据
bool uploadSingleData(float distanceCm, time_t timestamp) {
  // 检查 WiFi 连接
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  // 如果时间未同步，尝试同步时间
  if (!timeSynced) {
    syncNTPTime();
  }
  
  // 如果传入的时间戳为0，使用当前时间
  if (timestamp == 0) {
    timestamp = getCurrentTimestamp();
  }
  
  // 构建完整的 URL
  String url = String(apiBaseUrl) + "/device/ultrasonicSensor/" + String(ultrasonicSensorId) + "/";
  
  // 构建 JSON 数据（包含 currentDistance 和 dataUpdatedAt 字段）
  String payload = "{\"currentDistance\":";
  payload += String(distanceCm, 2);
  
  // 添加 dataUpdatedAt 字段
  if (timestamp > 0) {
    String dateTimeStr = formatDateTime(timestamp);
    if (dateTimeStr.length() > 0) {
      payload += ",\"dataUpdatedAt\":\"";
      payload += dateTimeStr;
      payload += "\"";
    }
  }
  
  payload += "}";
  
  // 判断 URL 是 HTTP 还是 HTTPS
  bool useHTTPS = url.startsWith("https://");

  HTTPClient http;
  http.setTimeout(10000);  // 设置HTTP超时
  
  // 根据协议类型选择客户端
  bool beginResult = false;
  if (useHTTPS) {
    static WiFiClientSecure secureClient;
    secureClient.setInsecure();  // 跳过证书验证（用于测试）
    secureClient.setTimeout(10000);
    beginResult = http.begin(secureClient, url);
  } else {
    static WiFiClient client;
    client.setTimeout(10000);
    beginResult = http.begin(client, url);
  }
  
  if (!beginResult) {
    return false;
  }

  // 设置请求头
  http.addHeader("X-API-Key", apiKey);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  
  int httpCode = http.PATCH(payload);
  // 记录错误字符串以便在 http.end() 后仍可打印
  String httpError = http.errorToString(httpCode);
  http.end();

  // 判断是否成功
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT || httpCode == 200 || httpCode == 204) {
    return true;
  }
  
  Serial.print("[HTTP] 上传失败，状态码: ");
  Serial.print(httpCode);
  Serial.print(" (");
  Serial.print(httpError);
  Serial.println(")");
  
  return false;
}

// 批量上传本地数据（每次上传多条数据，严格 FIFO 顺序）
void uploadLocalData() {
  // 防止重复触发
  if (isUploading) {
    return;
  }
  
  // 检查 WiFi 连接
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  // 检查是否有本地数据
  int storedCount = getStoredDataCount();
  if (storedCount == 0) {
    return;
  }
  
  isUploading = true;
  
  // 确定本次批量上传的条数（不超过配置的最大值，也不超过实际存储的数据量）
  int batchSize = (storedCount < BATCH_UPLOAD_SIZE) ? storedCount : BATCH_UPLOAD_SIZE;
  
  // 分配临时数组存储批量数据
  float* distances = new float[batchSize];
  time_t* timestamps = new time_t[batchSize];
  
  // 批量读取数据
  int readCount = readBatchDataFromStorage(distances, timestamps, batchSize);
  
  if (readCount == 0) {
    // 没有数据了
    delete[] distances;
    delete[] timestamps;
    isUploading = false;
    return;
  }
  
  Serial.print("\n[批量上传] 开始上传 ");
  Serial.print(readCount);
  Serial.print(" 条数据（剩余 ");
  Serial.print(storedCount);
  Serial.println(" 条）");
  
  // 逐条上传，记录成功和失败的数量
  int successCount = 0;
  int failCount = 0;
  
  for (int i = 0; i < readCount; i++) {
    Serial.print("[上传] 第 ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(readCount);
    Serial.print(" 条: ");
    Serial.print(distances[i], 2);
    Serial.print(" cm");
    if (timestamps[i] > 0) {
      Serial.print(", 时间: ");
      Serial.print(formatDateTime(timestamps[i]));
    }
    
    // 尝试上传
    if (uploadSingleData(distances[i], timestamps[i])) {
      successCount++;
      Serial.println(" ✓ 成功");
    } else {
      failCount++;
      Serial.println(" ✗ 失败");
      // 如果上传失败，停止后续上传，保留剩余数据等待下次重试
      Serial.println("[警告] 上传失败，停止本次批量上传，保留剩余数据");
      break;
    }
    
    // 每条数据之间稍作延时，避免请求过快
    delay(100);
  }
  
  // 删除成功上传的数据（只删除成功上传的部分）
  if (successCount > 0) {
    removeBatchDataFromStorage(successCount);
    Serial.print("[删除] 已删除 ");
    Serial.print(successCount);
    Serial.println(" 条已成功上传的数据");
  }
  
  // 释放临时数组
  delete[] distances;
  delete[] timestamps;
  
  // 显示统计信息
  int remainingCount = getStoredDataCount();
  Serial.print("[统计] 本次上传: 成功 ");
  Serial.print(successCount);
  Serial.print(" 条");
  if (failCount > 0) {
    Serial.print(", 失败 ");
    Serial.print(failCount);
    Serial.print(" 条（已保留）");
  }
  Serial.print(" | 剩余待上传: ");
  Serial.print(remainingCount);
  Serial.println(" 条\n");
  
  isUploading = false;
}

// 生成模拟距离数据
float generateSimulatedDistance() {
  // 生成 40.0~80.0 cm 之间的随机距离值
  float randomDistance = random(400, 801) / 10.0f;
  return randomDistance;
}

void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("ESP32 数据收集与上传程序 (ESP32_2)");
  Serial.println("========================================\n");

  // 初始化 LittleFS 文件系统
  Serial.println("[存储] 初始化 LittleFS 文件系统...");
  if (!LittleFS.begin(true)) {
    Serial.println("[错误] LittleFS 初始化失败");
  } else {
    Serial.println("[存储] ✓ LittleFS 初始化成功");
    
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;
    
    Serial.print("[存储] 总容量: ");
    Serial.print(totalBytes / 1024.0, 2);
    Serial.print(" KB, 已使用: ");
    Serial.print(usedBytes / 1024.0, 2);
    Serial.print(" KB, 可用: ");
    Serial.print(freeBytes / 1024.0, 2);
    Serial.println(" KB");
  }
  
  // 显示本地数据统计
  int storedCount = getStoredDataCount();
  if (storedCount > 0) {
    Serial.print("[存储] 发现 ");
    Serial.print(storedCount);
    Serial.println(" 条未上传的数据");
  } else {
    Serial.println("[存储] 无未上传的数据");
  }

  loadStoredWiFiCredentials();

  // 初始化 WiFi
  Serial.println("[WiFi] 初始化 WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  bool wifiConnected = connectWiFi();

  // 如果 WiFi 连接成功，同步 NTP 时间
  if (wifiConnected) {
    syncNTPTime();
  } else {
    Serial.println("[WiFi] Wi-Fi 连接失败，自动进入配置模式");
    startConfigPortal();
  }

  // 初始化数据收集时间
  lastCollectTime = 0;
  lastUploadCheckTime = 0;

  Serial.println("\n[系统] 初始化完成");
  Serial.println("[模式] 数据收集与智能上传模式：");
  Serial.println("  ✓ 每5秒收集一条数据（模拟长度 + 日期时间含时区）");
  Serial.println("  ✓ 智能上传策略：");
  Serial.println("    - 本地无待上传数据且网络正常 → 直接上传（不保存本地）");
  Serial.println("    - 本地有待上传数据或网络不可用 → 保存到本地");
  Serial.println("    - 直接上传失败 → 自动保存到本地等待后续上传");
  Serial.println("  ✓ 只要有网络且本地有数据，持续批量上传（不受5秒间隔限制）");
  Serial.println("  ✓ 严格 FIFO 顺序：先保存的数据先上传");
  Serial.print("  ✓ 批量上传：每次上传 ");
  Serial.print(BATCH_UPLOAD_SIZE);
  Serial.println(" 条数据，提高上传效率");
  Serial.println("  ✓ 上传成功后批量删除，失败则保留数据等待重试");
  Serial.print("[配置] API Key: ");
  Serial.println(apiKey);
  Serial.print("[配置] 传感器ID: ");
  Serial.println(ultrasonicSensorId);
  Serial.println("========================================\n");
}

void loop() {
  unsigned long now = millis();

  if (configPortalActive) {
    configServer.handleClient();
    if (CONFIG_PORTAL_TIMEOUT_MS > 0 &&
        (now - configPortalLastActivity) > CONFIG_PORTAL_TIMEOUT_MS) {
      Serial.println("[WiFi] 配置模式超时，重启设备重试");
      delay(1000);
      ESP.restart();
    }
    delay(20);
    return;
  }
  
  // 检查 WiFi 连接状态
  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  // 如果 WiFi 从断开变为连接，同步时间
  if (isConnected && !wasConnected) {
    Serial.println("\n[网络] WiFi 已恢复连接");
    if (!timeSynced) {
      syncNTPTime();
    }
  }
  
  // 如果 WiFi 断开，尝试重连
  if (!isConnected) {
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt >= 10000) {  // 每10秒尝试重连一次
      lastReconnectAttempt = now;
      Serial.println("[WiFi] 连接断开，正在重连...");
      if (!connectWiFi()) {
        Serial.println("[WiFi] 无法重连，进入 Wi-Fi 配置模式");
        startConfigPortal();
      }
      if (WiFi.status() == WL_CONNECTED && !timeSynced) {
        syncNTPTime();
      }
    }
  }
  
  wasConnected = isConnected;

  // 持续上传本地数据（只要有网络和数据就上传，不受5秒间隔限制）
  if (isConnected && !isUploading) {
    if (now - lastUploadCheckTime >= uploadCheckInterval) {
      lastUploadCheckTime = now;
      uploadLocalData();
    }
  }

  // 周期性收集数据（每5秒收集一条）
  if (now - lastCollectTime >= collectInterval) {
    lastCollectTime = now;
    
    // 生成模拟数据
    float simulatedDistance = generateSimulatedDistance();
    
    // 获取当前时间戳
    time_t currentTimestamp = getCurrentTimestamp();
    
    // 同步时间（如果需要）
    if (isConnected && !timeSynced) {
      syncNTPTime();
      // 重新获取时间戳
      currentTimestamp = getCurrentTimestamp();
    }
    
    Serial.print("\n[收集] 距离: ");
    Serial.print(simulatedDistance, 2);
    Serial.print(" cm");
    if (currentTimestamp > 0) {
      Serial.print(", 时间: ");
      Serial.print(formatDateTime(currentTimestamp));
    } else {
      Serial.print(", 时间: (未同步)");
    }
    
    // 检查本地是否有待上传的数据
    int storedCount = getStoredDataCount();
    bool hasPendingData = (storedCount > 0);
    
    // 如果本地没有待上传数据且网络正常，优先直接上传
    if (!hasPendingData && isConnected && !isUploading) {
      Serial.print(" | 本地无待上传数据，尝试直接上传...");
      
      // 尝试直接上传
      if (uploadSingleData(simulatedDistance, currentTimestamp)) {
        Serial.println(" ✓ 直接上传成功（未保存到本地）");
      } else {
        // 上传失败，保存到本地
        Serial.print(" ✗ 直接上传失败，保存到本地");
        if (saveDataToStorage(simulatedDistance, currentTimestamp)) {
          Serial.print(" ✓ 已保存");
          Serial.print(" (本地共 ");
          Serial.print(getStoredDataCount());
          Serial.print(" 条)");
        } else {
          Serial.print(" ✗ 保存失败");
        }
        Serial.println();
      }
    } else {
      // 本地有待上传数据或网络不可用，保存到本地（保持原有逻辑）
      if (hasPendingData) {
        Serial.print(" | 本地有待上传数据，保存到本地");
      } else if (!isConnected) {
        Serial.print(" | 网络不可用，保存到本地");
      } else {
        Serial.print(" | 上传进行中，保存到本地");
      }
      
      if (saveDataToStorage(simulatedDistance, currentTimestamp)) {
        Serial.print(" ✓ 已保存");
        Serial.print(" (本地共 ");
        Serial.print(getStoredDataCount());
        Serial.print(" 条)");
      } else {
        Serial.print(" ✗ 保存失败");
      }
      Serial.println();
    }
    
    // 每20条数据输出一次统计信息
    static int collectCount = 0;
    collectCount++;
    if (collectCount % 20 == 0) {
      int currentStoredCount = getStoredDataCount();
      Serial.print("\n[统计] 已收集 ");
      Serial.print(collectCount);
      Serial.print(" 条数据，本地存储: ");
      Serial.print(currentStoredCount);
      Serial.println(" 条");
    }
  }

  delay(50);  // 短暂延时
  yield();  // 喂看门狗
}

