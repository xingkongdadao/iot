/*
 * ESP32 模拟数据上传到后台（实时上传模式）
 * ----------------------------------------
 * 功能：
 * - 连接 WiFi 并每5秒收集一次模拟超声波数据
 * - 有网络时立即上传数据到后台
 * - 网络不可用时保存到本地作为备份
 * - 简化代码，避免卡顿
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// PSRAM 使用标志（用于诊断信息）
bool usePsram = false;

// WiFi 配置（与 test_1.ino 保持一致）
const char* ssid = "GOGOTRANS";
const char* password = "18621260183";

// API 配置（与 test_1.ino 保持一致）
// 本地测试地址：http://192.168.100.193:8001/api
// 生产环境地址：https://manage.gogotrans.com/api
const char* apiBaseUrl = "http://192.168.100.193:8001/api";  // 本地测试地址
// const char* apiBaseUrl = "https://manage.gogotrans.com/api";  // 生产环境地址
const char* apiKey = "mcu_8312592b29fd4c68a0e01336cf26f438";
const char ultrasonicSensorId[] = "8ea58210-c649-11f0-afa3-da038af01e18";

// NTP 时间同步配置
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;  // 东七区（UTC+7），单位：秒
const int daylightOffset_sec = 0;     // 夏令时偏移（中国不使用夏令时）
bool timeSynced = false;              // 时间是否已同步

// 数据收集间隔（毫秒）
const unsigned long collectInterval = 5000;  // 每5秒收集一条数据
unsigned long lastCollectTime = 0;

// 批量上传间隔（毫秒）- 有网络时持续上传历史数据
const unsigned long batchUploadInterval = 1000;  // 每1秒检查一次是否有历史数据需要上传
unsigned long lastBatchUploadTime = 0;
static bool isBatchUploading = false;  // 批量上传进行中标志，防止重复触发

// 数据结构：存储距离和时间戳
struct DataRecord {
  float distance;
  time_t timestamp;  // Unix 时间戳
};

// 本地数据缓存（用于网络断开时暂存数据）
// 注意：有网络时数据会立即上传，不会进入缓存
const int bufferSize = 50;  // 增大缓存容量，可保存更多离线数据
DataRecord dataBuffer[bufferSize];
int bufferIndex = 0;  // 当前缓存位置（循环缓冲区）
int dataCount = 0;    // 已存储的数据条数

// 持久化存储配置（使用 LittleFS）
const char* dataFilePath = "/sensor_data.json";  // 数据文件路径
const size_t minFreeHeap = 20000;                // 最小可用堆内存阈值（字节），低于此值则删除最旧数据
const size_t JSON_DOC_SIZE = 1048576;            // JSON 文档大小（字节），使用数组格式每条记录约20字节，可存储约52400条数据
                                                  // ESP32-S3 N16R8 有 8MB PSRAM，可支持更大容量
                                                  // 推荐配置：
                                                  //   32768   (32KB)  - 约 1630条  - 适合小容量场景（每分钟1条可存约1.1天）
                                                  //   65536   (64KB)  - 约 3270条  - 适合中等容量场景（每分钟1条可存约2.3天）
                                                  //   131072  (128KB) - 约 6550条  - 适合常规场景（每分钟1条可存约4.5天）
                                                  //   262144  (256KB) - 约 13100条 - 常规场景（每分钟1条可存约9.1天）
                                                  //   524288  (512KB) - 约 26200条 - 大容量场景（每分钟1条可存约18.2天）
                                                  //   1048576 (1MB)   - 约 52400条 - 当前配置，超大容量（每分钟1条可存约36.4天）
                                                  //   2097152 (2MB)   - 约 104800条 - 极限配置（每分钟1条可存约72.8天，需测试稳定性）

void connectWiFi() {
  Serial.print("[WiFi] 正在连接: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);

  int attempts = 0;
  const int maxAttempts = 20;  // 最多尝试20次（10秒）
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    yield();  // 喂看门狗
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
  Serial.print("[时间] NTP 服务器: ");
  Serial.println(ntpServer);
  
  // 配置时间（ESP32支持多个NTP服务器作为备用）
  // 如果第一个服务器失败，会自动尝试其他服务器
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.nist.gov", "time.google.com");
  
  // 等待时间同步（最多等待10秒）
  int attempts = 0;
  time_t now = 0;
  while (attempts < 20) {
    delay(500);
    yield();
    now = time(nullptr);
    if (now > 1000000000) {
      break;  // 时间同步成功
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
    Serial.print("[时间] ✗ 时间同步失败 (尝试了 ");
    Serial.print(attempts);
    Serial.print(" 次，当前时间戳: ");
    Serial.print(now);
    Serial.println(")");
    Serial.println("[时间] 提示: 请检查网络连接和NTP服务器可访问性");
    timeSynced = false;
  }
}

// 获取当前时间戳
time_t getCurrentTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000) {
    // 时间未同步，返回0或使用 millis() 作为相对时间
    return 0;
  }
  return now;
}

// 注意：ESP32 Arduino 核心的 malloc 实现会自动使用 PSRAM（如果可用）进行大块分配
// DynamicJsonDocument 内部使用 malloc，所以对于大块内存（如 JSON_DOC_SIZE = 1MB）
// 应该自动使用 PSRAM，无需特殊处理
// 我们只需要确保 PSRAM 已启用（在 setup() 中已处理）

// 格式化时间为 ISO 8601 格式字符串（YYYY-MM-DDTHH:MM:SS+08:00，包含时区）
String formatDateTime(time_t timestamp) {
  if (timestamp == 0) {
    return "";  // 时间未同步时返回空字符串
  }
  
  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);
  
  // 计算时区偏移（小时和分钟）
  int offsetHours = gmtOffset_sec / 3600;
  int offsetMinutes = abs((gmtOffset_sec % 3600) / 60);
  
  // 格式化时区字符串（+08:00 或 -05:00）
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
  // 记录分配 JSON 文档前的内存
  size_t freeHeapBefore = ESP.getFreeHeap();
  
  // 读取现有数据
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  
  // 记录分配 JSON 文档后的内存
  size_t freeHeapAfter = ESP.getFreeHeap();
  size_t memoryUsedByDoc = freeHeapBefore - freeHeapAfter;
  
  // 如果内存使用异常（分配失败或使用过多），输出警告
  if (memoryUsedByDoc > JSON_DOC_SIZE * 1.5) {
    Serial.print("[警告] JSON 文档内存使用异常: 预计 ");
    Serial.print(JSON_DOC_SIZE / 1024.0, 2);
    Serial.print(" KB，实际使用 ");
    Serial.print(memoryUsedByDoc / 1024.0, 2);
    Serial.println(" KB");
  }
  
  int storedCount = 0;
  int originalCount = 0;
  
  if (LittleFS.exists(dataFilePath)) {
    File file = LittleFS.open(dataFilePath, "r");
    if (file) {
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      if (!error) {
        storedCount = doc["c"].as<int>();
        originalCount = storedCount;
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
  // 计算10%限制阈值（用于警告，但不阻止删除）
  int warningThreshold = originalCount > 0 ? max(1, originalCount / 10) : 0;
  bool warningShown = false;  // 标记是否已显示10%警告
  
  while (freeHeap < minFreeHeap && storedCount > 0) {
    if (deletedCount == 0) {
      Serial.print("[警告] 可用内存不足 (");
      Serial.print(freeHeap);
      Serial.print(" 字节 < ");
      Serial.print(minFreeHeap);
      Serial.println(" 字节)，开始删除最旧的数据");
    }
    
    // 当达到10%限制时给出警告，但继续删除
    if (deletedCount >= warningThreshold && !warningShown && originalCount > 0) {
      Serial.print("[警告] 已删除 ");
      Serial.print(deletedCount);
      Serial.print(" 条数据（占原有数据的 ");
      Serial.print((deletedCount * 100.0 / originalCount), 1);
      Serial.println("%），但内存仍不足，继续删除直到内存充足");
      warningShown = true;
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
  
  // 记录保存后的内存（JSON 文档即将被释放）
  size_t freeHeapFinal = ESP.getFreeHeap();
  
  // 如果内存低于阈值，输出警告
  if (freeHeapFinal < minFreeHeap) {
    Serial.print("[警告] 保存数据后可用内存: ");
    Serial.print(freeHeapFinal);
    Serial.print(" 字节 (");
    Serial.print(freeHeapFinal / 1024.0, 2);
    Serial.print(" KB) < 阈值 ");
    Serial.print(minFreeHeap);
    Serial.print(" 字节 (");
    Serial.print(minFreeHeap / 1024.0, 2);
    Serial.println(" KB)");
  }
  
  return true;
}

// 从持久化存储读取所有数据到内存缓存
int loadDataFromStorage() {
  if (!LittleFS.exists(dataFilePath)) {
    return 0;
  }
  
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    Serial.println("[错误] 无法打开数据文件");
    return 0;
  }
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("[错误] JSON 解析失败: ");
    Serial.println(error.c_str());
    return 0;
  }
  
  int storedCount = doc["c"].as<int>();
  
  if (storedCount == 0) {
    return 0;
  }
  
  // 读取数据到内存缓存
  dataCount = 0;
  bufferIndex = 0;
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  for (int i = 0; i < dataArray.size() && i < bufferSize; i++) {
    JsonArray record = dataArray[i];
    if (record.size() >= 2) {
      float dist = record[0].as<float>();
      time_t ts = (time_t)record[1].as<uint64_t>();
      
      if (dist > 0 || ts > 0) {  // 有效数据
        dataBuffer[dataCount].distance = dist;
        dataBuffer[dataCount].timestamp = ts;
        dataCount++;
      }
    }
  }
  
  return dataCount;
}

// 从持久化存储删除已上传的数据（从开头删除指定数量）
void removeDataFromStorage(int count) {
  if (count <= 0) return;
  
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
    Serial.print("[错误] JSON 解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  
  int storedCount = doc["c"].as<int>();
  
  if (count >= storedCount) {
    // 删除所有数据
    doc["c"] = 0;
    doc["a"] = JsonArray();
  } else {
    // 删除前count条数据
    JsonArray dataArray = doc["a"].as<JsonArray>();
    for (int i = 0; i < count && dataArray.size() > 0; i++) {
      dataArray.remove(0);
    }
    doc["c"] = storedCount - count;
  }
  
  // 保存回文件
  file = LittleFS.open(dataFilePath, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

// 从持久化存储删除指定索引的数据（按索引从大到小删除，避免索引变化）
void removeDataFromStorageByIndices(int* indices, int count) {
  if (count <= 0 || indices == nullptr) return;
  
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
    Serial.print("[错误] JSON 解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  
  // 对索引进行排序（从大到小），这样删除时索引不会变化
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (indices[j] < indices[j + 1]) {
        int temp = indices[j];
        indices[j] = indices[j + 1];
        indices[j + 1] = temp;
      }
    }
  }
  
  // 从后往前删除（从索引大的开始删除）
  int actualDeleted = 0;
  for (int i = 0; i < count; i++) {
    int idx = indices[i];
    if (idx >= 0 && idx < dataArray.size()) {
      dataArray.remove(idx);
      actualDeleted++;
    }
  }
  
  int storedCount = doc["c"].as<int>();
  doc["c"] = (storedCount > actualDeleted) ? (storedCount - actualDeleted) : 0;
  
  // 保存回文件
  file = LittleFS.open(dataFilePath, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
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

// 存储数据到本地缓存（内存+持久化存储）
void storeDataLocally(float distanceCm, time_t timestamp) {
  // 保存到内存缓存
  dataBuffer[bufferIndex].distance = distanceCm;
  dataBuffer[bufferIndex].timestamp = timestamp;
  bufferIndex = (bufferIndex + 1) % bufferSize;  // 循环索引
  
  if (dataCount < bufferSize) {
    dataCount++;  // 缓存未满时，增加计数
  }
  // 缓存满时，dataCount 保持为 bufferSize，新数据会覆盖旧数据
  
  // 保存到持久化存储
  bool saveSuccess = saveDataToStorage(distanceCm, timestamp);
  
  // 每10条数据输出一次详细信息，其他时候只输出简要信息
  static int saveCount = 0;
  saveCount++;
  int storedCount = getStoredDataCount();
  
  if (saveCount % 10 == 0) {
    // 详细输出
    Serial.print("[存储] ✓ 第 ");
    Serial.print(saveCount);
    Serial.print(" 条数据已保存（内存: ");
    Serial.print(dataCount);
    Serial.print("/");
    Serial.print(bufferSize);
    Serial.print(", 持久化: ");
    Serial.print(storedCount);
    Serial.print(" 条）: ");
    Serial.print(distanceCm, 2);
    Serial.print(" cm");
    if (timestamp > 0) {
      Serial.print(", 时间: ");
      Serial.print(formatDateTime(timestamp));
    }
    if (!saveSuccess) {
      Serial.print(" [警告: 保存失败]");
    }
    Serial.println();
    
    // 显示存储统计
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    Serial.print("[存储统计] 文件系统: 已用 ");
    Serial.print(LittleFS.usedBytes() / 1024.0, 2);
    Serial.print(" KB / 总计 ");
    Serial.print(LittleFS.totalBytes() / 1024.0, 2);
    Serial.print(" KB, 可用 ");
    Serial.print(freeBytes / 1024.0, 2);
    Serial.print(" KB");
    Serial.print(" | 数据量: ");
    Serial.print(storedCount);
    Serial.print(" 条");
    Serial.print(" | 平均每条: ");
    if (storedCount > 0) {
      Serial.print((LittleFS.usedBytes() / (float)storedCount), 1);
      Serial.print(" 字节");
    }
    Serial.println();
  } else {
    // 简要输出（每1条）
    if (saveCount % 1 == 0) {
      Serial.print(".");
      if (saveCount % 50 == 0) {
        Serial.println();
        Serial.print("[存储] 已保存 ");
        Serial.print(saveCount);
        Serial.print(" 条，本地存储: ");
        Serial.print(storedCount);
        Serial.println(" 条");
      }
    }
  }
}

// 批量上传数据（从持久化存储读取，按时间戳排序，最早的数据优先上传）
bool uploadBatchData() {
  // 防止重复触发
  if (isBatchUploading) {
    return false;
  }
  
  isBatchUploading = true;
  
  // 从持久化存储获取数据条数
  int storedCount = getStoredDataCount();
  
  if (storedCount == 0) {
    Serial.println("[上传] 持久化存储为空，无需上传");
    // 清空内存缓存（以防不一致）
    bufferIndex = 0;
    dataCount = 0;
    isBatchUploading = false;
    return false;
  }

  Serial.println("\n========== 开始批量上传 ==========");
  Serial.print("[信息] 准备上传 ");
  Serial.print(storedCount);
  Serial.println(" 条数据（从持久化存储，按时间排序）");
  
  // 检查 WiFi 连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[错误] WiFi 未连接，无法上传");
    isBatchUploading = false;
    return false;
  }

  // 从持久化存储读取所有数据
  File file = LittleFS.open(dataFilePath, "r");
  if (!file) {
    Serial.println("[错误] 无法打开数据文件");
    isBatchUploading = false;
    return false;
  }
  
  // 检查文件大小
  size_t fileSize = file.size();
  Serial.print("[调试] 数据文件大小: ");
  Serial.print(fileSize);
  Serial.print(" 字节 (");
  Serial.print(fileSize / 1024.0, 2);
  Serial.println(" KB)");
  
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("[错误] JSON 解析失败: ");
    Serial.println(error.c_str());
    Serial.print("[错误] 错误代码: ");
    Serial.println((int)error.code());
    Serial.print("[错误] 错误位置: ");
    Serial.println(error.offset());
    isBatchUploading = false;
    return false;
  }
  
  // 检查 JSON 文档结构
  Serial.print("[调试] JSON 文档容量: ");
  Serial.print(doc.capacity());
  Serial.print(" 字节 (");
  Serial.print(doc.capacity() / 1024.0, 2);
  Serial.println(" KB)");
  Serial.print("[调试] JSON 文档内存使用: ");
  Serial.print(doc.memoryUsage());
  Serial.print(" 字节 (");
  Serial.print(doc.memoryUsage() / 1024.0, 2);
  Serial.println(" KB)");
  
  // 检查是否包含 "a" 字段
  if (!doc.containsKey("a")) {
    Serial.println("[错误] JSON 文档中不存在 'a' 字段！");
    Serial.print("[调试] JSON 文档包含的键: ");
    for (JsonPair kv : doc.as<JsonObject>()) {
      Serial.print(kv.key().c_str());
      Serial.print(" ");
    }
    Serial.println();
    isBatchUploading = false;
    return false;
  }
  
  // 检查 "a" 字段的类型
  if (!doc["a"].is<JsonArray>()) {
    Serial.print("[错误] 'a' 字段不是数组类型！实际类型: ");
    if (doc["a"].is<JsonObject>()) Serial.println("对象");
    else if (doc["a"].is<const char*>()) Serial.println("字符串");
    else if (doc["a"].is<int>()) Serial.println("整数");
    else if (doc["a"].is<float>()) Serial.println("浮点数");
    else if (doc["a"].is<bool>()) Serial.println("布尔值");
    else if (doc["a"].is<nullptr_t>()) Serial.println("null");
    else Serial.println("未知类型");
    isBatchUploading = false;
    return false;
  }
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  
  // 添加调试信息：检查 JSON 结构
  int jsonCount = doc["c"].as<int>();
  int actualArraySize = dataArray.size();
  Serial.print("[调试] JSON 文档中记录的数量 (c): ");
  Serial.println(jsonCount);
  Serial.print("[调试] JSON 数据数组实际大小: ");
  Serial.println(actualArraySize);
  
  // 检查一致性
  if (jsonCount != actualArraySize) {
    Serial.print("[警告] 数据不一致！JSON 计数 (c)=");
    Serial.print(jsonCount);
    Serial.print(", 实际数组大小=");
    Serial.println(actualArraySize);
    Serial.println("[警告] 将使用实际数组大小进行处理");
    
    // 如果数组大小为 0，尝试检查 JSON 文档是否溢出
    if (actualArraySize == 0 && jsonCount > 0) {
      Serial.println("[错误] ⚠️ 严重问题：数组大小为 0 但计数不为 0！");
      Serial.println("[错误] 可能原因：");
      Serial.println("[错误] 1. JSON 文档容量不足，解析时被截断");
      Serial.println("[错误] 2. JSON 文档损坏");
      Serial.println("[错误] 3. 内存不足导致解析失败");
      Serial.print("[错误] 建议：增加 JSON_DOC_SIZE 到至少 ");
      Serial.print((fileSize * 1.2) / 1024.0, 0);
      Serial.println(" KB");
      isBatchUploading = false;
      return false;
    }
  }
  
  // 将所有数据读取到临时数组，并记录原始索引
  const int maxRecords = dataArray.size();
  DataRecord* records = new DataRecord[maxRecords];
  int* originalIndices = new int[maxRecords];
  int validCount = 0;
  int skippedCount = 0;
  int invalidFormatCount = 0;
  
  for (int i = 0; i < dataArray.size(); i++) {
    JsonArray record = dataArray[i];
    if (record.size() < 2) {
      // 数据格式不正确，跳过
      invalidFormatCount++;
      if (invalidFormatCount <= 3) {
        Serial.print("[调试] 记录 [");
        Serial.print(i);
        Serial.print("] 格式不正确，大小: ");
        Serial.println(record.size());
      }
      continue;
    }
    float dist = record[0].as<float>();
    time_t ts = (time_t)record[1].as<uint64_t>();
    
    // 验证数据有效性：距离或时间戳至少有一个不为0（与loadDataFromStorage逻辑一致）
    if (dist > 0 || ts > 0) {
      records[validCount].distance = dist;
      records[validCount].timestamp = ts;
      originalIndices[validCount] = i;
      validCount++;
    } else {
      // 如果 dist == 0 && ts == 0，跳过这条无效数据
      skippedCount++;
      if (skippedCount <= 3) {
        Serial.print("[调试] 记录 [");
        Serial.print(i);
        Serial.print("] 无效 (dist=0 && ts=0): dist=");
        Serial.print(dist, 2);
        Serial.print(", ts=");
        Serial.println(ts);
      }
    }
  }
  
  // 输出统计信息
  Serial.print("[调试] 总记录数: ");
  Serial.print(dataArray.size());
  Serial.print(", 有效记录数: ");
  Serial.print(validCount);
  Serial.print(", 格式错误: ");
  Serial.print(invalidFormatCount);
  Serial.print(", 无效数据 (dist=0 && ts=0): ");
  Serial.println(skippedCount);
  
  // 如果有效数据为0，输出前几条数据的详细信息
  if (validCount == 0 && dataArray.size() > 0) {
    Serial.println("[调试] 详细检查前10条数据:");
    for (int i = 0; i < min(10, (int)dataArray.size()); i++) {
      JsonArray record = dataArray[i];
      Serial.print("  [");
      Serial.print(i);
      Serial.print("] 记录大小: ");
      Serial.print(record.size());
      if (record.size() >= 2) {
        float dist = record[0].as<float>();
        time_t ts = (time_t)record[1].as<uint64_t>();
        Serial.print(", dist=");
        Serial.print(dist, 2);
        Serial.print(", ts=");
        Serial.print(ts);
        Serial.print(", 验证结果: ");
        if (dist > 0 || ts > 0) {
          Serial.println("有效");
        } else {
          Serial.println("无效 (dist=0 && ts=0)");
        }
      } else {
        Serial.println(", 格式错误");
      }
    }
  }
  
  // 按时间戳排序（最早的数据在前）
  // 排序规则：时间戳为0的数据视为最旧，排在最前面；其他数据按时间戳升序排列
  Serial.println("[排序] 正在按时间戳排序数据（最早优先）...");
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = 0; j < validCount - i - 1; j++) {
      time_t ts1 = records[j].timestamp;
      time_t ts2 = records[j + 1].timestamp;
      
      bool needSwap = false;
      
      // 处理时间戳为0的情况：0视为最旧
      if (ts1 == 0 && ts2 == 0) {
        // 两个都是0，保持原顺序（稳定排序）
        needSwap = false;
      } else if (ts1 == 0) {
        // ts1是0，应该排在最前面，不需要交换
        needSwap = false;
      } else if (ts2 == 0) {
        // ts2是0，应该排在最前面，需要交换
        needSwap = true;
      } else {
        // 两个都不是0，按时间戳大小比较
        needSwap = (ts1 > ts2);
      }
      
      if (needSwap) {
        // 交换数据记录
        DataRecord temp = records[j];
        records[j] = records[j + 1];
        records[j + 1] = temp;
        
        // 交换原始索引
        int tempIdx = originalIndices[j];
        originalIndices[j] = originalIndices[j + 1];
        originalIndices[j + 1] = tempIdx;
      }
    }
  }
  
  Serial.print("[排序] 排序完成，共 ");
  Serial.print(validCount);
  Serial.println(" 条有效数据");
  
  // 按排序后的顺序上传数据，并记录已上传成功的原始索引
  int successCount = 0;
  int failCount = 0;
  int* uploadedIndices = new int[validCount];  // 记录已上传成功的原始索引
  
  for (int i = 0; i < validCount; i++) {
    float dist = records[i].distance;
    time_t ts = records[i].timestamp;
    
    Serial.print("\n[上传] 第 ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(validCount);
    Serial.print(" 条数据（时间排序后）: ");
    Serial.print(dist, 2);
    Serial.print(" cm");
    if (ts > 0) {
      Serial.print(", 时间: ");
      Serial.print(formatDateTime(ts));
    } else {
      Serial.print(", 时间: (未同步)");
    }
    Serial.println();
    
    if (uploadSingleData(dist, ts)) {
      // 记录已上传成功的原始索引
      uploadedIndices[successCount] = originalIndices[i];
      successCount++;
    } else {
      failCount++;
      // 如果上传失败，停止上传，保留剩余数据
      Serial.println("[警告] 上传失败，停止批量上传，保留剩余数据");
      break;
    }
    delay(200);  // 每条数据之间稍作延时
  }
  
  // 删除已成功上传的数据（根据原始索引精确删除）
  if (successCount > 0) {
    Serial.print("[删除] 正在删除 ");
    Serial.print(successCount);
    Serial.println(" 条已上传的数据...");
    removeDataFromStorageByIndices(uploadedIndices, successCount);
  }
  
  // 释放临时数组
  delete[] records;
  delete[] originalIndices;
  delete[] uploadedIndices;

  // 清空内存缓存（保持一致性）
  bufferIndex = 0;
  dataCount = 0;
  
  Serial.println("\n========== 批量上传完成 ==========");
  Serial.print("[统计] 成功上传到后台: ");
  Serial.print(successCount);
  Serial.print(" 条距离数据");
  if (failCount > 0) {
    Serial.print(", 失败: ");
    Serial.print(failCount);
    Serial.print(" 条（已保留在本地）");
  }
  Serial.println();
  Serial.print("[存储] 剩余本地数据: ");
  Serial.print(getStoredDataCount());
  Serial.println(" 条");
  Serial.println("========================================\n");
  
  isBatchUploading = false;
  return (failCount == 0);
}

// 上传单条数据
bool uploadSingleData(float distanceCm, time_t timestamp) {
  Serial.println("\n========== 开始上传 ==========");
  
  // 检查 WiFi 连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[错误] WiFi 未连接，无法上传");
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
  Serial.print("[URL] 目标地址: ");
  Serial.println(url);
  
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
  
  Serial.print("[数据] 距离值: ");
  Serial.print(distanceCm, 2);
  Serial.println(" cm");
  if (timestamp > 0) {
    Serial.print("[数据] 获取时间: ");
    Serial.println(formatDateTime(timestamp));
  }
  Serial.print("[JSON] 请求体: ");
  Serial.println(payload);
  Serial.println("[字段] 将保存到后台字段: currentDistance, dataUpdatedAt");
  
  // 判断 URL 是 HTTP 还是 HTTPS
  bool useHTTPS = url.startsWith("https://");
  Serial.print("[协议] 检测到协议: ");
  Serial.println(useHTTPS ? "HTTPS" : "HTTP");

  HTTPClient http;
  http.setTimeout(10000);  // 设置HTTP超时
  
  Serial.println("[连接] 正在连接服务器...");
  
  // 根据协议类型选择客户端（客户端对象需要在整个请求期间保持有效）
  bool beginResult = false;
  if (useHTTPS) {
    // 使用 HTTPS 客户端
    static WiFiClientSecure secureClient;  // 使用 static 确保对象在整个请求期间有效
    secureClient.setInsecure();  // 跳过证书验证（用于测试）
    secureClient.setTimeout(10000);  // 设置10秒超时
    beginResult = http.begin(secureClient, url);
  } else {
    // 使用普通 HTTP 客户端
    static WiFiClient client;  // 使用 static 确保对象在整个请求期间有效
    client.setTimeout(10000);  // 设置10秒超时
    beginResult = http.begin(client, url);
  }
  
  if (!beginResult) {
    Serial.println("[错误] 无法初始化 HTTP 连接");
    return false;
  }

  // 设置请求头
  http.addHeader("X-API-Key", apiKey);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  
  // 调试：显示使用的 API Key（部分隐藏）
  Serial.print("[调试] 使用的 API Key: ");
  if (strlen(apiKey) > 8) {
    Serial.print(apiKey[0]);
    Serial.print(apiKey[1]);
    Serial.print("...");
    Serial.print(apiKey[strlen(apiKey)-2]);
    Serial.print(apiKey[strlen(apiKey)-1]);
  } else {
    Serial.print(apiKey);
  }
  Serial.println();
  
  Serial.println("[请求] 发送 PATCH 请求...");
  
  int httpCode = http.PATCH(payload);
  
  Serial.print("[响应] HTTP 状态码: ");
  Serial.println(httpCode);
  
  // 获取响应内容
  String response = http.getString();
  
  if (httpCode > 0) {
    Serial.print("[响应] 服务器返回: ");
    if (response.length() > 0) {
      Serial.println(response);
    } else {
      Serial.println("(无响应内容)");
    }
  } else {
    Serial.print("[错误] 请求失败，错误代码: ");
    Serial.println(httpCode);
    Serial.print("[错误] 错误信息: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();

  // 判断是否成功
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT || httpCode == 200 || httpCode == 204) {
    Serial.print("[成功] ✓ 距离值 ");
    Serial.print(distanceCm, 2);
    Serial.print(" cm");
    if (timestamp > 0) {
      Serial.print(" 和时间 ");
      Serial.print(formatDateTime(timestamp));
    }
    Serial.println(" 已成功保存到后台");
    Serial.println("========== 上传结束 ==========\n");
    return true;
  } else if (httpCode == 401) {
    Serial.println("[失败] ✗ 认证失败 (401)");
    Serial.println("[提示] 可能的原因：");
    Serial.println("  1. API Key 无效或已过期");
    Serial.println("  2. 请在后台检查 API Key 是否正确");
    Serial.println("  3. 当前使用的 API Key: ultrasonic_sensor_c2fd255e");
    Serial.println("========== 上传结束 ==========\n");
    return false;
  } else {
    Serial.print("[失败] ✗ 上传失败，HTTP状态码: ");
    Serial.println(httpCode);
    Serial.println("========== 上传结束 ==========\n");
    return false;
  }
}

float generateSimulatedDistance() {
  // 生成 40.0~80.0 cm 之间的随机距离值
  // random(400, 801) 生成 400-800 的整数，除以 10.0 得到 40.0-80.0 的浮点数
  float randomDistance = random(400, 801) / 10.0f;
  return randomDistance;
}

void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);  // 等待串口稳定
  
  // 检查并启用 PSRAM
  #ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
      usePsram = true;
      Serial.println("[PSRAM] ✓ PSRAM 已检测到并启用");
      Serial.print("[PSRAM] 总容量: ");
      Serial.print(ESP.getPsramSize());
      Serial.print(" 字节 (");
      Serial.print(ESP.getPsramSize() / 1024.0 / 1024.0, 2);
      Serial.println(" MB)");
      Serial.print("[PSRAM] 可用: ");
      Serial.print(ESP.getFreePsram());
      Serial.print(" 字节 (");
      Serial.print(ESP.getFreePsram() / 1024.0 / 1024.0, 2);
      Serial.println(" MB)");
      Serial.println("[PSRAM] JSON 文档将使用 PSRAM 分配");
    } else {
      Serial.println("[PSRAM] ✗ PSRAM 未检测到，将使用内部 SRAM");
      usePsram = false;
    }
  #else
    Serial.println("[PSRAM] 此开发板不支持 PSRAM，将使用内部 SRAM");
    usePsram = false;
  #endif
  
  // 关闭开发板上的多色灯（RGB LED）
  // ESP32 开发板上的 RGB LED 通常连接到 GPIO 2
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  
  // 如果 LED 连接到其他引脚，也可以尝试以下引脚：
  // pinMode(48, OUTPUT);  // 某些新型号 ESP32 的 RGB LED
  // digitalWrite(48, LOW);
  
  // 初始化随机数种子（使用未连接的模拟引脚噪声作为种子）
  randomSeed(analogRead(0) + millis());
  
  Serial.println("\n========================================");
  Serial.println("ESP32 数据上传程序启动");
  Serial.println("========================================\n");

  // 初始化 LittleFS 文件系统
  Serial.println("[存储] 初始化 LittleFS 文件系统...");
  if (!LittleFS.begin(true)) {  // true 表示如果文件系统不存在则格式化
    Serial.println("[错误] LittleFS 初始化失败");
  } else {
    Serial.println("[存储] ✓ LittleFS 初始化成功");
    
    // 显示文件系统容量信息
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;
    
    Serial.print("[存储] 总容量: ");
    Serial.print(totalBytes);
    Serial.print(" 字节 (");
    Serial.print(totalBytes / 1024.0, 2);
    Serial.println(" KB)");
    
    Serial.print("[存储] 已使用: ");
    Serial.print(usedBytes);
    Serial.print(" 字节 (");
    Serial.print(usedBytes / 1024.0, 2);
    Serial.println(" KB)");
    
    Serial.print("[存储] 可用空间: ");
    Serial.print(freeBytes);
    Serial.print(" 字节 (");
    Serial.print(freeBytes / 1024.0, 2);
    Serial.println(" KB)");
    
    // 计算理论最大存储条数（基于 JSON 文档大小限制）
    // 使用数组格式每条记录约 20 字节（[distance, timestamp]），JSON 结构开销约 10 字节
    const size_t jsonOverhead = 10;   // JSON 结构开销
    const size_t recordSize = 20;     // 每条记录的平均大小（数组格式：[distance, timestamp]）
    int maxRecords = (JSON_DOC_SIZE - jsonOverhead) / recordSize;
    
    Serial.print("[存储] 当前配置最大可存储: ");
    Serial.print(maxRecords);
    Serial.print(" 条数据（JSON 文档大小: ");
    Serial.print(JSON_DOC_SIZE);
    Serial.println(" 字节）");
    
    // 计算不同采集频率下的存储天数
    Serial.println("[存储] 存储容量分析（基于当前配置）:");
    const int recordsPerMinute = 1;   // 每分钟存储条数
    const int recordsPerHour = recordsPerMinute * 60;
    const int recordsPerDay = recordsPerHour * 24;
    
    float daysAt1PerMin = (float)maxRecords / recordsPerDay;
    Serial.print("  • 每分钟 1 条: 可存储约 ");
    Serial.print(daysAt1PerMin, 1);
    Serial.println(" 天");
    
    float daysAt1Per5Min = (float)maxRecords / (recordsPerDay / 5);
    Serial.print("  • 每 5 分钟 1 条: 可存储约 ");
    Serial.print(daysAt1Per5Min, 1);
    Serial.println(" 天");
    
    float daysAt1Per10Min = (float)maxRecords / (recordsPerDay / 10);
    Serial.print("  • 每 10 分钟 1 条: 可存储约 ");
    Serial.print(daysAt1Per10Min, 1);
    Serial.println(" 天");
    
    float daysAt1PerHour = (float)maxRecords / recordsPerHour;
    Serial.print("  • 每小时 1 条: 可存储约 ");
    Serial.print(daysAt1PerHour, 1);
    Serial.println(" 天");
    
    // 计算如果使用全部文件系统容量可存储多少条数据
    if (freeBytes > JSON_DOC_SIZE) {
      int theoreticalMax = (freeBytes - jsonOverhead) / recordSize;
      Serial.print("[存储] 理论最大容量（使用全部文件系统）: ");
      Serial.print(theoreticalMax);
      Serial.print(" 条数据（每分钟 1 条可存储约 ");
      Serial.print((float)theoreticalMax / recordsPerDay, 1);
      Serial.println(" 天）");
      Serial.println("[提示] 要使用更大容量，需要增大 JSON 文档大小限制");
    }
  }
  
  int storedCount = getStoredDataCount();
  
  if (storedCount > 0) {
    Serial.print("[存储] 发现 ");
    Serial.print(storedCount);
    Serial.println(" 条未上传的数据");
    // 加载数据到内存（但不立即上传，等待网络连接）
    loadDataFromStorage();
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
    Serial.println("[系统] WiFi 已连接，准备收集数据");
    syncNTPTime();
  } else {
    Serial.println("[系统] WiFi 未连接，数据将先存储在本地");
  }

  // 初始化数据收集时间
  lastCollectTime = 0;  // 立即开始第一次数据收集

  Serial.println("\n[系统] 初始化完成");
  Serial.println("[模式] 实时上传模式：");
  Serial.println("  ✓ 每5秒收集一条数据");
  Serial.println("  ✓ 有网络时立即上传到后台");
  Serial.println("  ✓ 网络不可用时保存到本地（作为备份）");
  Serial.println("  ✓ 数据保存在持久化存储（断电不丢失）");
  Serial.println("[数据] 模拟距离值：随机生成 40.0~80.0 cm");
  Serial.println("[字段] 距离值将保存到后台 currentDistance 字段");
  Serial.println("[字段] 数据获取时间将保存到后台 dataUpdatedAt 字段");
  Serial.print("[配置] API Key: ");
  Serial.println(apiKey);
  Serial.print("[配置] 传感器ID: ");
  Serial.println(ultrasonicSensorId);
  Serial.print("[配置] 内存缓存容量: ");
  Serial.print(bufferSize);
  Serial.println(" 条");
  Serial.print("[配置] 持久化存储: LittleFS 文件系统（内存容量报警时自动删除最旧数据）");
  Serial.print("，最小可用内存阈值: ");
  Serial.print(minFreeHeap);
  Serial.println(" 字节");
  
  // 内存诊断信息
  Serial.println("\n[内存] 系统内存诊断:");
  size_t totalHeap = ESP.getHeapSize();
  size_t freeHeap = ESP.getFreeHeap();
  size_t largestFreeBlock = ESP.getMaxAllocHeap();
  
  Serial.print("  • 总堆内存: ");
  Serial.print(totalHeap);
  Serial.print(" 字节 (");
  Serial.print(totalHeap / 1024.0, 2);
  Serial.println(" KB)");
  
  Serial.print("  • 当前可用堆内存: ");
  Serial.print(freeHeap);
  Serial.print(" 字节 (");
  Serial.print(freeHeap / 1024.0, 2);
  Serial.println(" KB)");
  
  Serial.print("  • 最大可分配块: ");
  Serial.print(largestFreeBlock);
  Serial.print(" 字节 (");
  Serial.print(largestFreeBlock / 1024.0, 2);
  Serial.println(" KB)");
  
  Serial.print("  • JSON 文档大小: ");
  Serial.print(JSON_DOC_SIZE);
  Serial.print(" 字节 (");
  Serial.print(JSON_DOC_SIZE / 1024.0, 2);
  Serial.println(" KB)");
  
  // 分析内存是否足够（考虑 PSRAM）
  #ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
      size_t freePsram = ESP.getFreePsram();
      Serial.print("[分析] JSON 文档将使用 PSRAM 分配（ESP32 自动路由大块分配到 PSRAM）");
      Serial.print("\n[分析] 当前 PSRAM 可用: ");
      Serial.print(freePsram);
      Serial.print(" 字节 (");
      Serial.print(freePsram / 1024.0 / 1024.0, 2);
      Serial.println(" MB)");
      
      if (freePsram < JSON_DOC_SIZE) {
        Serial.print("[警告] ⚠️ JSON 文档大小 (");
        Serial.print(JSON_DOC_SIZE / 1024.0 / 1024.0, 2);
        Serial.print(" MB) 超过可用 PSRAM (");
        Serial.print(freePsram / 1024.0 / 1024.0, 2);
        Serial.println(" MB)！");
        Serial.println("[警告] 这可能导致 JSON 文档分配失败，建议减小 JSON_DOC_SIZE");
      } else {
        size_t estimatedFreePsramAfterAlloc = freePsram - JSON_DOC_SIZE;
        Serial.print("[分析] 分配 JSON 文档后预计可用 PSRAM: ");
        Serial.print(estimatedFreePsramAfterAlloc);
        Serial.print(" 字节 (");
        Serial.print(estimatedFreePsramAfterAlloc / 1024.0 / 1024.0, 2);
        Serial.println(" MB)");
        Serial.println("[信息] ✓ JSON 文档使用 PSRAM，不会影响内部 SRAM 堆内存");
      }
    } else {
      // PSRAM 不可用，使用内部 SRAM
      if (largestFreeBlock < JSON_DOC_SIZE) {
        Serial.print("[警告] ⚠️ JSON 文档大小 (");
        Serial.print(JSON_DOC_SIZE / 1024.0, 2);
        Serial.print(" KB) 超过最大可分配块 (");
        Serial.print(largestFreeBlock / 1024.0, 2);
        Serial.println(" KB)！");
        Serial.println("[警告] 这可能导致 JSON 文档分配失败，建议减小 JSON_DOC_SIZE");
      } else {
        size_t estimatedFreeAfterAlloc = freeHeap - JSON_DOC_SIZE;
        Serial.print("[分析] 分配 JSON 文档后预计可用内存: ");
        Serial.print(estimatedFreeAfterAlloc);
        Serial.print(" 字节 (");
        Serial.print(estimatedFreeAfterAlloc / 1024.0, 2);
        Serial.println(" KB)");
        
        if (estimatedFreeAfterAlloc < minFreeHeap) {
          Serial.print("[警告] ⚠️ 分配 JSON 文档后，可用内存 (");
          Serial.print(estimatedFreeAfterAlloc / 1024.0, 2);
          Serial.print(" KB) 将低于阈值 (");
          Serial.print(minFreeHeap / 1024.0, 2);
          Serial.println(" KB)！");
          Serial.println("[警告] 系统可能会频繁触发数据删除机制");
        } else {
          Serial.println("[信息] ✓ 分配 JSON 文档后，可用内存仍高于阈值，预计安全");
        }
      }
    }
  #else
    // 不支持 PSRAM，使用内部 SRAM
    if (largestFreeBlock < JSON_DOC_SIZE) {
      Serial.print("[警告] ⚠️ JSON 文档大小 (");
      Serial.print(JSON_DOC_SIZE / 1024.0, 2);
      Serial.print(" KB) 超过最大可分配块 (");
      Serial.print(largestFreeBlock / 1024.0, 2);
      Serial.println(" KB)！");
      Serial.println("[警告] 这可能导致 JSON 文档分配失败，建议减小 JSON_DOC_SIZE");
    } else {
      size_t estimatedFreeAfterAlloc = freeHeap - JSON_DOC_SIZE;
      Serial.print("[分析] 分配 JSON 文档后预计可用内存: ");
      Serial.print(estimatedFreeAfterAlloc);
      Serial.print(" 字节 (");
      Serial.print(estimatedFreeAfterAlloc / 1024.0, 2);
      Serial.println(" KB)");
      
      if (estimatedFreeAfterAlloc < minFreeHeap) {
        Serial.print("[警告] ⚠️ 分配 JSON 文档后，可用内存 (");
        Serial.print(estimatedFreeAfterAlloc / 1024.0, 2);
        Serial.print(" KB) 将低于阈值 (");
        Serial.print(minFreeHeap / 1024.0, 2);
        Serial.println(" KB)！");
        Serial.println("[警告] 系统可能会频繁触发数据删除机制");
      } else {
        Serial.println("[信息] ✓ 分配 JSON 文档后，可用内存仍高于阈值，预计安全");
      }
    }
  #endif
  
  // 检查 PSRAM（如果可用）
  #ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
      size_t psramSize = ESP.getPsramSize();
      size_t freePsram = ESP.getFreePsram();
      Serial.print("  • PSRAM 总容量: ");
      Serial.print(psramSize);
      Serial.print(" 字节 (");
      Serial.print(psramSize / 1024.0 / 1024.0, 2);
      Serial.println(" MB)");
      Serial.print("  • PSRAM 可用: ");
      Serial.print(freePsram);
      Serial.print(" 字节 (");
      Serial.print(freePsram / 1024.0 / 1024.0, 2);
      Serial.println(" MB)");
      Serial.println("[提示] ESP32 Arduino 核心会自动将大块内存分配（>4KB）路由到 PSRAM");
      Serial.println("[提示] JSON 文档（1MB）应该自动使用 PSRAM 分配");
      Serial.print("[提示] 分配 JSON 文档后，PSRAM 可用量将减少约 ");
      Serial.print(JSON_DOC_SIZE / 1024.0 / 1024.0, 2);
      Serial.println(" MB");
    }
  #endif
  
  Serial.println();
  
  Serial.print("[配置] 数据收集间隔: ");
  Serial.print(collectInterval);
  Serial.print(" 毫秒 (");
  Serial.print(collectInterval / 1000.0, 1);
  Serial.println(" 秒)");
  Serial.print("[配置] 预计每小时收集数据: ");
  Serial.print((3600.0 * 1000.0 / collectInterval));
  Serial.println(" 条");
  Serial.println("========================================\n");
}

void loop() {
  unsigned long now = millis();
  
  // 检查 WiFi 连接状态
  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  // 如果 WiFi 从断开变为连接，同步时间并尝试上传历史数据
  if (isConnected && !wasConnected) {
    Serial.println("\n[网络] WiFi 已恢复连接");
    // 同步时间
    if (!timeSynced) {
      syncNTPTime();
    }
    // 显示本地数据统计并尝试上传历史数据
    int storedCount = getStoredDataCount();
    if (storedCount > 0) {
      Serial.print("[网络] 本地存储了 ");
      Serial.print(storedCount);
      Serial.println(" 条历史数据，开始上传...");
      uploadBatchData();
    }
  }
  
  // 如果 WiFi 断开，尝试重连
  if (!isConnected) {
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt >= 10000) {  // 每10秒尝试重连一次
      lastReconnectAttempt = now;
      Serial.println("[WiFi] 连接断开，正在重连...");
      connectWiFi();
      // 重连成功后同步时间
      if (WiFi.status() == WL_CONNECTED && !timeSynced) {
        syncNTPTime();
      }
    }
  }
  
  wasConnected = isConnected;

  // 如果有网络连接，持续检查并上传历史数据（每1秒检查一次）
  if (isConnected && !isBatchUploading) {
    if (now - lastBatchUploadTime >= batchUploadInterval) {
      lastBatchUploadTime = now;
      
      // 检查是否有历史数据需要上传
      int storedCount = getStoredDataCount();
      if (storedCount > 0) {
        Serial.print("\n[批量上传] 检测到 ");
        Serial.print(storedCount);
        Serial.println(" 条历史数据，开始上传...");
        uploadBatchData();
      }
    }
  }

  // 周期性收集数据（每5秒收集一条）
  if (now - lastCollectTime >= collectInterval) {
    lastCollectTime = now;
    
    // 生成模拟数据
    float simulatedDistance = generateSimulatedDistance();
    
    // 获取当前时间戳
    time_t currentTimestamp = getCurrentTimestamp();
    
    // 如果有网络，立即尝试上传
    if (isConnected) {
      // 同步时间（如果需要）
      if (!timeSynced) {
        syncNTPTime();
      }
      
      // 立即尝试上传
      Serial.print("\n[数据] 距离: ");
      Serial.print(simulatedDistance, 2);
      Serial.print(" cm");
      if (currentTimestamp > 0) {
        Serial.print(", 时间: ");
        Serial.print(formatDateTime(currentTimestamp));
      }
      Serial.println();
      
      if (uploadSingleData(simulatedDistance, currentTimestamp)) {
        Serial.println("[成功] ✓ 数据已实时上传到后台");
      } else {
        // 上传失败，保存到本地作为备份
        Serial.println("[警告] 上传失败，保存到本地作为备份");
        storeDataLocally(simulatedDistance, currentTimestamp);
      }
    } else {
      // 没有网络，保存到本地
      Serial.print("\n[数据] 距离: ");
      Serial.print(simulatedDistance, 2);
      Serial.print(" cm");
      if (currentTimestamp > 0) {
        Serial.print(", 时间: ");
        Serial.print(formatDateTime(currentTimestamp));
      }
      Serial.println(" (网络不可用，已保存到本地)");
      storeDataLocally(simulatedDistance, currentTimestamp);
    }
    
    // 每20条数据输出一次统计信息
    static int collectCount = 0;
    collectCount++;
    if (collectCount % 20 == 0) {
      int storedCount = getStoredDataCount();
      Serial.print("\n[统计] 已收集 ");
      Serial.print(collectCount);
      Serial.print(" 条数据");
      if (storedCount > 0) {
        Serial.print("，本地备份: ");
        Serial.print(storedCount);
        Serial.print(" 条");
      }
      Serial.println();
    }
  }

  delay(50);  // 短暂延时
  yield();  // 喂看门狗
}

