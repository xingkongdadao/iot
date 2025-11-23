/*
 * ESP32 本地存储容量测试程序（ESP32_3）
 * ----------------------------------------
 * 功能：
 * - 每秒生成100条数据（每10ms生成1条）
 * - 数据类型：模拟长度（float）+ 日期时间（time_t）
 * - 所有数据均保存到本地 LittleFS
 * - 每生成100条数据后，输出统计信息（内存、总数据量）
 * - 记录何时开始删除旧数据以保证设备正常运行
 * - 测试本地可以存储多少条数据
 */

#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// NTP 时间同步配置（可选，用于生成真实时间戳）
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;  // 东七区（UTC+7），单位：秒
const int daylightOffset_sec = 0;     // 夏令时偏移
bool timeSynced = false;              // 时间是否已同步

// 数据生成配置
const unsigned long generateInterval = 10;  // 每10ms生成1条数据（每秒100条）
unsigned long lastGenerateTime = 0;

// 持久化存储配置（使用 LittleFS）
const char* dataFilePath = "/sensor_data.json";  // 数据文件路径
const size_t minFreeHeap = 20000;                // 最小可用堆内存阈值（字节）
const size_t JSON_DOC_SIZE = 1048576;            // JSON 文档大小（1MB），可存储约52400条数据

// 统计变量
unsigned long totalGeneratedCount = 0;      // 总共生成的数据条数
unsigned long totalStoredCount = 0;         // 当前存储的数据条数
bool firstDeletionOccurred = false;         // 是否已发生首次删除
unsigned long firstDeletionAtCount = 0;     // 首次删除时的总数据量
int deletionBatchCount = 0;                // 删除批次计数

// 获取当前时间戳（如果时间未同步，返回模拟时间戳）
time_t getCurrentTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000) {
    // 时间未同步，返回基于启动时间的模拟时间戳
    static unsigned long startTime = millis();
    return (time_t)(startTime / 1000) + 1000000000;  // 从2001年开始的秒数
  }
  return now;
}

// 生成模拟距离数据
float generateSimulatedDistance() {
  // 生成 40.0~80.0 cm 之间的随机距离值
  float randomDistance = random(400, 801) / 10.0f;
  return randomDistance;
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
  if (!doc.containsKey("a")) {
    doc["c"] = 0;
    doc["a"] = JsonArray();
    storedCount = 0;
  }
  
  // 检查可用堆内存，如果低于阈值则删除最旧的数据（FIFO）
  size_t freeHeap = ESP.getFreeHeap();
  int deletedCount = 0;
  
  JsonArray dataArray = doc["a"].as<JsonArray>();
  while (freeHeap < minFreeHeap && dataArray.size() > 0) {
    if (deletedCount == 0) {
      // 记录首次删除
      if (!firstDeletionOccurred) {
        firstDeletionOccurred = true;
        firstDeletionAtCount = totalGeneratedCount;
        Serial.println("\n========================================");
        Serial.println("[重要] 首次删除旧数据事件发生！");
        Serial.println("========================================");
      }
      deletionBatchCount++;
      Serial.print("\n[警告] 可用内存不足 (");
      Serial.print(freeHeap);
      Serial.print(" 字节 < ");
      Serial.print(minFreeHeap);
      Serial.print(" 字节)，开始删除最旧的数据");
      Serial.print(" [批次 #");
      Serial.print(deletionBatchCount);
      Serial.println("]");
    }
    
    // 删除最旧的数据（数组第一个元素）
    dataArray.remove(0);
    storedCount--;
    deletedCount++;
    
    // 再次检查内存
    freeHeap = ESP.getFreeHeap();
  }
  
  if (deletedCount > 0) {
    Serial.print("[删除] 已删除 ");
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
  
  // 更新存储计数
  totalStoredCount = dataArray.size();
  
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
  
  // 返回实际数组大小，而不是计数器（确保准确性）
  if (doc.containsKey("a")) {
    JsonArray dataArray = doc["a"].as<JsonArray>();
    return dataArray.size();
  }
  
  return 0;
}

// 输出统计信息（每100条数据后调用）
void printStatistics() {
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t usedHeap = totalHeap - freeHeap;
  int storedCount = getStoredDataCount();
  
  Serial.println("\n========================================");
  Serial.println("[统计] 每100条数据统计信息");
  Serial.println("========================================");
  Serial.print("总生成数据量: ");
  Serial.print(totalGeneratedCount);
  Serial.println(" 条");
  Serial.print("当前存储数据量: ");
  Serial.print(storedCount);
  Serial.println(" 条");
  Serial.print("已删除数据量: ");
  Serial.print(totalGeneratedCount - storedCount);
  Serial.println(" 条");
  
  if (firstDeletionOccurred) {
    Serial.print("首次删除发生时间: 第 ");
    Serial.print(firstDeletionAtCount);
    Serial.println(" 条数据");
    Serial.print("删除批次次数: ");
    Serial.print(deletionBatchCount);
    Serial.println(" 次");
  } else {
    Serial.println("首次删除: 尚未发生");
  }
  
  Serial.println("\n[内存信息]");
  Serial.print("  总堆内存: ");
  Serial.print(totalHeap / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  已用堆内存: ");
  Serial.print(usedHeap / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  可用堆内存: ");
  Serial.print(freeHeap / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  内存使用率: ");
  Serial.print((usedHeap * 100.0 / totalHeap), 2);
  Serial.println(" %");
  
  // LittleFS 存储信息
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  
  Serial.println("\n[存储信息]");
  Serial.print("  LittleFS 总容量: ");
  Serial.print(totalBytes / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  LittleFS 已使用: ");
  Serial.print(usedBytes / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  LittleFS 可用: ");
  Serial.print(freeBytes / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  LittleFS 使用率: ");
  Serial.print((usedBytes * 100.0 / totalBytes), 2);
  Serial.println(" %");
  
  // 计算平均每条数据占用的存储空间
  if (storedCount > 0) {
    float avgBytesPerRecord = (float)usedBytes / storedCount;
    Serial.print("  平均每条数据: ");
    Serial.print(avgBytesPerRecord, 2);
    Serial.println(" 字节");
    
    // 估算最大可存储数据量
    unsigned long estimatedMaxRecords = (unsigned long)(totalBytes / avgBytesPerRecord);
    Serial.print("  估算最大存储量: ");
    Serial.print(estimatedMaxRecords);
    Serial.println(" 条");
  }
  
  Serial.println("========================================\n");
}

void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("ESP32 本地存储容量测试程序 (ESP32_3)");
  Serial.println("========================================");
  Serial.println("\n[模式] 存储压力测试模式：");
  Serial.println("  ✓ 每秒生成100条数据（每10ms生成1条）");
  Serial.println("  ✓ 数据类型：模拟长度（float）+ 日期时间（time_t）");
  Serial.println("  ✓ 所有数据均保存到本地 LittleFS");
  Serial.println("  ✓ 每生成100条数据后输出统计信息");
  Serial.println("  ✓ 记录何时开始删除旧数据");
  Serial.println("  ✓ 测试本地可以存储多少条数据");
  Serial.println("========================================\n");

  // 初始化 LittleFS 文件系统
  Serial.println("[存储] 初始化 LittleFS 文件系统...");
  if (!LittleFS.begin(true)) {
    Serial.println("[错误] LittleFS 初始化失败");
    Serial.println("[错误] 程序无法继续运行");
    while (1) {
      delay(1000);
    }
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
  
  // 显示初始内存信息
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  
  Serial.println("\n[内存] 初始内存状态：");
  Serial.print("  总堆内存: ");
  Serial.print(totalHeap / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  可用堆内存: ");
  Serial.print(freeHeap / 1024.0, 2);
  Serial.println(" KB");
  Serial.print("  最小可用堆内存阈值: ");
  Serial.print(minFreeHeap / 1024.0, 2);
  Serial.println(" KB");
  
  // 检查是否有旧数据
  int storedCount = getStoredDataCount();
  if (storedCount > 0) {
    Serial.print("\n[警告] 发现 ");
    Serial.print(storedCount);
    Serial.println(" 条旧数据，建议先清空存储");
    Serial.println("[提示] 如需清空，请删除文件系统或重新格式化");
  } else {
    Serial.println("\n[信息] 存储为空，开始全新测试");
  }
  
  // 初始化统计变量
  totalGeneratedCount = 0;
  totalStoredCount = 0;
  firstDeletionOccurred = false;
  firstDeletionAtCount = 0;
  deletionBatchCount = 0;
  
  // 初始化数据生成时间
  lastGenerateTime = 0;
  
  Serial.println("\n[系统] 初始化完成，开始生成数据...");
  Serial.println("========================================\n");
}

void loop() {
  unsigned long now = millis();
  
  // 每10ms生成1条数据（每秒100条）
  if (now - lastGenerateTime >= generateInterval) {
    lastGenerateTime = now;
    
    // 生成模拟数据
    float simulatedDistance = generateSimulatedDistance();
    time_t currentTimestamp = getCurrentTimestamp();
    
    // 保存到本地
    if (saveDataToStorage(simulatedDistance, currentTimestamp)) {
      totalGeneratedCount++;
      
      // 每生成100条数据后输出统计信息
      if (totalGeneratedCount % 100 == 0) {
        printStatistics();
      }
    } else {
      Serial.print("[错误] 第 ");
      Serial.print(totalGeneratedCount + 1);
      Serial.println(" 条数据保存失败");
    }
  }
  
  delay(1);  // 短暂延时，避免过于频繁
  yield();   // 喂看门狗
}

