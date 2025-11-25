#include "Uploader.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "Config.h"
#include "StorageManager.h"
#include "TimeUtils.h"

bool isUploading = false;

bool uploadSingleData(float distanceCm, time_t timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!timeSynced) {
    syncNTPTime();
  }

  if (timestamp == 0) {
    timestamp = getCurrentTimestamp();
  }

  String url = String(API_BASE_URL) + "/device/ultrasonicSensor/" + String(ULTRASONIC_SENSOR_ID) + "/";

  String payload = "{\"currentDistance\":";
  payload += String(distanceCm, 2);

  if (timestamp > 0) {
    String dateTimeStr = formatDateTime(timestamp);
    if (dateTimeStr.length() > 0) {
      payload += ",\"dataUpdatedAt\":\"";
      payload += dateTimeStr;
      payload += "\"";
    }
  }

  payload += "}";

  bool useHTTPS = url.startsWith("https://");

  HTTPClient http;
  http.setTimeout(10000);

  bool beginResult = false;
  if (useHTTPS) {
    static WiFiClientSecure secureClient;
    secureClient.setInsecure();
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

  http.addHeader("X-API-Key", API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int httpCode = http.PATCH(payload);
  String httpError = http.errorToString(httpCode);
  http.end();

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

void uploadLocalData() {
  if (isUploading) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  int storedCount = getStoredDataCount();
  if (storedCount == 0) {
    return;
  }

  isUploading = true;

  int batchSize = (storedCount < BATCH_UPLOAD_SIZE) ? storedCount : BATCH_UPLOAD_SIZE;

  float* distances = new float[batchSize];
  time_t* timestamps = new time_t[batchSize];

  int readCount = readBatchDataFromStorage(distances, timestamps, batchSize);

  if (readCount == 0) {
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

    if (uploadSingleData(distances[i], timestamps[i])) {
      successCount++;
      Serial.println(" ✓ 成功");
    } else {
      failCount++;
      Serial.println(" ✗ 失败");
      Serial.println("[警告] 上传失败，停止本次批量上传，保留剩余数据");
      break;
    }

    delay(100);
  }

  if (successCount > 0) {
    removeBatchDataFromStorage(successCount);
    Serial.print("[删除] 已删除 ");
    Serial.print(successCount);
    Serial.println(" 条已成功上传的数据");
  }

  delete[] distances;
  delete[] timestamps;

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

