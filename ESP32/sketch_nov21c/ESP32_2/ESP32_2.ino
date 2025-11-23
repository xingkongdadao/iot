/*
 * ESP32 数据收集与上传程序（ESP32_2）
 * ----------------------------------------
 * 功能：
 * - 每5秒收集一条数据（模拟长度 + 日期时间含时区）
 * - 每条数据均先保存在本地
 * - 只要有网络且本地有数据，持续上传（不受5秒间隔限制）
 * - 严格 FIFO 顺序：先保存的数据先上传，上传成功后立即删除
 * - 每次只上传一条数据，确保数据顺序和可靠性
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// WiFi 配置（与 ESP32_1.ino 保持一致）
const char* ssid = "GOGOTRANS";
const char* password = "18621260183";

// API 配置（与 ESP32_1.ino 保持一致）
// 本地测试地址：http://192.168.100.193:8001/api
// 生产环境地址：https://manage.gogotrans.com/api
const char* apiBaseUrl = "http://192.168.100.193:8001/api";  // 本地测试地址
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

// 持久化存储配置（使用 LittleFS）
const char* dataFilePath = "/sensor_data.json";  // 数据文件路径
const size_t minFreeHeap = 20000;                // 最小可用堆内存阈值（字节）
const size_t JSON_DOC_SIZE = 1048576;            // JSON 文档大小（1MB），可存储约52400条数据

// 连接 WiFi
void connectWiFi() {
  Serial.print("[WiFi] 正在连接: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);

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
  } else {
    Serial.println("[WiFi] ✗ 连接失败，将在loop中继续尝试重连...");
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
      if (!error) {
        storedCount = doc["c"].as<int>();
      }
    }
  }
  
  // 如果文件不存在或解析失败，初始化 JSON 结构
  if (!doc.containsKey("a")) {
    doc["c"] = 0;
    doc["a"] = JsonArray();
  }
  
  // 检查可用堆内存，如果低于阈值则删除最旧的数据（FIFO）
  size_t freeHeap = ESP.getFreeHeap();
  int deletedCount = 0;
  
  while (freeHeap < minFreeHeap && storedCount > 0) {
    if (deletedCount == 0) {
      Serial.print("[警告] 可用内存不足 (");
      Serial.print(freeHeap);
      Serial.print(" 字节 < ");
      Serial.print(minFreeHeap);
      Serial.println(" 字节)，开始删除最旧的数据");
    }
    
    // 删除最旧的数据（数组第一个元素）
    JsonArray dataArray = doc["a"].as<JsonArray>();
    if (dataArray.size() > 0) {
      dataArray.remove(0);
      storedCount--;
      deletedCount++;
    }
    
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
  JsonArray dataArray = doc["a"].to<JsonArray>();
  JsonArray newRecord = dataArray.createNestedArray();
  newRecord.add(distanceCm);
  newRecord.add((uint64_t)timestamp);
  doc["c"] = storedCount + 1;
  
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

// 获取持久化存储中的数据条数
int getStoredDataCount() {
  if (!LittleFS.exists(dataFilePath)) {
    return 0;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    return 0;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    return 0;
  }
  
  return doc["c"].as<int>();
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
  
  int storedCount = doc["c"].as<int>();
  
  if (storedCount <= 0) {
    return;
  }
  
  // 删除第一条数据
  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.size() > 0) {
    dataArray.remove(0);
    doc["c"] = storedCount - 1;
  }
  
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
  http.end();

  // 判断是否成功
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT || httpCode == 200 || httpCode == 204) {
    return true;
  }
  
  return false;
}

// 上传本地数据（每次只上传一条最早的数据，严格 FIFO 顺序）
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
  
  // 读取第一条数据（最早保存的数据，FIFO）
  float distance;
  time_t timestamp;
  
  if (!readFirstDataFromStorage(distance, timestamp)) {
    // 没有数据了
    isUploading = false;
    return;
  }
  
  // 显示上传信息
  Serial.print("[上传] 正在上传最早的数据: ");
  Serial.print(distance, 2);
  Serial.print(" cm");
  if (timestamp > 0) {
    Serial.print(", 时间: ");
    Serial.print(formatDateTime(timestamp));
  }
  Serial.print(" (剩余 ");
  Serial.print(storedCount);
  Serial.print(" 条)");
  
  // 尝试上传
  if (uploadSingleData(distance, timestamp)) {
    // 上传成功，立即删除这条数据
    removeFirstDataFromStorage();
    Serial.println(" ✓ 上传成功，已删除");
    
    // 显示剩余数据条数
    int remainingCount = getStoredDataCount();
    if (remainingCount > 0) {
      Serial.print("[上传] 剩余 ");
      Serial.print(remainingCount);
      Serial.println(" 条数据待上传");
    } else {
      Serial.println("[上传] 所有数据已上传完成");
    }
  } else {
    // 上传失败，保留数据，等待下次重试
    Serial.println(" ✗ 上传失败，保留数据等待下次重试");
  }
  
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

  // 初始化 WiFi
  Serial.println("[WiFi] 初始化 WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  connectWiFi();

  // 如果 WiFi 连接成功，同步 NTP 时间
  if (WiFi.status() == WL_CONNECTED) {
    syncNTPTime();
  }

  // 初始化数据收集时间
  lastCollectTime = 0;
  lastUploadCheckTime = 0;

  Serial.println("\n[系统] 初始化完成");
  Serial.println("[模式] 数据收集与上传模式：");
  Serial.println("  ✓ 每5秒收集一条数据（模拟长度 + 日期时间含时区）");
  Serial.println("  ✓ 每条数据均先保存在本地");
  Serial.println("  ✓ 只要有网络且本地有数据，持续上传（不受5秒间隔限制）");
  Serial.println("  ✓ 严格 FIFO 顺序：先保存的数据先上传");
  Serial.println("  ✓ 每次只上传一条数据，上传成功后立即删除");
  Serial.print("[配置] API Key: ");
  Serial.println(apiKey);
  Serial.print("[配置] 传感器ID: ");
  Serial.println(ultrasonicSensorId);
  Serial.println("========================================\n");
}

void loop() {
  unsigned long now = millis();
  
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
      connectWiFi();
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
    
    // 保存到本地（每条数据都先保存）
    Serial.print("\n[收集] 距离: ");
    Serial.print(simulatedDistance, 2);
    Serial.print(" cm");
    if (currentTimestamp > 0) {
      Serial.print(", 时间: ");
      Serial.print(formatDateTime(currentTimestamp));
    } else {
      Serial.print(", 时间: (未同步)");
    }
    
    if (saveDataToStorage(simulatedDistance, currentTimestamp)) {
      Serial.print(" ✓ 已保存到本地");
      Serial.print(" (本地共 ");
      Serial.print(getStoredDataCount());
      Serial.print(" 条)");
    } else {
      Serial.print(" ✗ 保存失败");
    }
    Serial.println();
    
    // 每20条数据输出一次统计信息
    static int collectCount = 0;
    collectCount++;
    if (collectCount % 20 == 0) {
      int storedCount = getStoredDataCount();
      Serial.print("\n[统计] 已收集 ");
      Serial.print(collectCount);
      Serial.print(" 条数据，本地存储: ");
      Serial.print(storedCount);
      Serial.println(" 条");
    }
  }

  delay(50);  // 短暂延时
  yield();  // 喂看门狗
}

