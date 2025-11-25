#include "StorageManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "../config/Config.h"

bool saveDataToStorage(float distanceCm, time_t timestamp) {
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  int storedCount = 0;

  if (LittleFS.exists(DATA_FILE_PATH)) {
    File file = LittleFS.open(DATA_FILE_PATH, "r");
    if (file) {
      DeserializationError error = deserializeJson(doc, file);
      file.close();
      if (!error && doc.containsKey("a")) {
        JsonArray tempArray = doc["a"].as<JsonArray>();
        storedCount = tempArray.size();
      }
    }
  }

  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.isNull()) {
    doc.remove("a");
    dataArray = doc.createNestedArray("a");
    doc["c"] = 0;
    storedCount = 0;
  }

  size_t freeHeap = ESP.getFreeHeap();
  int deletedCount = 0;

  while (freeHeap < MIN_FREE_HEAP && dataArray.size() > 0) {
    if (deletedCount == 0) {
      Serial.print("[警告] 可用内存不足 (");
      Serial.print(freeHeap);
      Serial.print(" 字节 < ");
      Serial.print(MIN_FREE_HEAP);
      Serial.println(" 字节)，开始删除最旧的数据");
    }

    dataArray.remove(0);
    storedCount--;
    deletedCount++;

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

  JsonArray newRecord = dataArray.createNestedArray();
  newRecord.add(distanceCm);
  newRecord.add((uint64_t)timestamp);
  doc["c"] = dataArray.size();

  File file = LittleFS.open(DATA_FILE_PATH, "w");
  if (!file) {
    Serial.println("[错误] 无法打开文件进行写入");
    return false;
  }

  serializeJson(doc, file);
  file.close();

  return true;
}

int getStoredDataCount() {
  if (!LittleFS.exists(DATA_FILE_PATH)) {
    return 0;
  }

  File file = LittleFS.open(DATA_FILE_PATH, "r");
  if (!file) {
    return 0;
  }

  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    return 0;
  }

  if (doc.containsKey("a")) {
    JsonArray dataArray = doc["a"].as<JsonArray>();
    return dataArray.size();
  }

  return 0;
}

bool readFirstDataFromStorage(float& distance, time_t& timestamp) {
  if (!LittleFS.exists(DATA_FILE_PATH)) {
    return false;
  }

  File file = LittleFS.open(DATA_FILE_PATH, "r");
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

  JsonArray record = dataArray[0];
  if (record.size() >= 2) {
    distance = record[0].as<float>();
    timestamp = (time_t)record[1].as<uint64_t>();
    return true;
  }

  return false;
}

void removeFirstDataFromStorage() {
  if (!LittleFS.exists(DATA_FILE_PATH)) {
    return;
  }

  File file = LittleFS.open(DATA_FILE_PATH, "r");
  if (!file) {
    return;
  }

  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    return;
  }

  if (!doc.containsKey("a")) {
    return;
  }

  JsonArray dataArray = doc["a"].as<JsonArray>();
  if (dataArray.size() == 0) {
    return;
  }

  dataArray.remove(0);
  doc["c"] = dataArray.size();

  file = LittleFS.open(DATA_FILE_PATH, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

int readBatchDataFromStorage(float* distances, time_t* timestamps, int maxCount) {
  if (!LittleFS.exists(DATA_FILE_PATH)) {
    return 0;
  }

  File file = LittleFS.open(DATA_FILE_PATH, "r");
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

  int readCount = (arraySize < maxCount) ? arraySize : maxCount;

  for (int i = 0; i < readCount; i++) {
    JsonArray record = dataArray[i];
    if (record.size() >= 2) {
      distances[i] = record[0].as<float>();
      timestamps[i] = (time_t)record[1].as<uint64_t>();
    } else {
      return i;
    }
  }

  return readCount;
}

void removeBatchDataFromStorage(int count) {
  if (!LittleFS.exists(DATA_FILE_PATH) || count <= 0) {
    return;
  }

  File file = LittleFS.open(DATA_FILE_PATH, "r");
  if (!file) {
    return;
  }

  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    return;
  }

  if (!doc.containsKey("a")) {
    return;
  }

  JsonArray dataArray = doc["a"].as<JsonArray>();
  int arraySize = dataArray.size();
  if (arraySize == 0) {
    return;
  }

  int deleteCount = (arraySize < count) ? arraySize : count;

  for (int i = 0; i < deleteCount; i++) {
    dataArray.remove(0);
  }

  doc["c"] = dataArray.size();

  file = LittleFS.open(DATA_FILE_PATH, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

