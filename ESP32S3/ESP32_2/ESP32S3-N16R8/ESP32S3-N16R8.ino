#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "config/Config.h"
#include "collector/DataCollector.h"
#include "storage/StorageManager.h"
#include "time/TimeUtils.h"
#include "upload/Uploader.h"
#include "network/WifiManager.h"

// Arduino 构建系统不会自动编译子目录中的 .cpp 文件，将其直接包含进来
#include "collector/DataCollector.cpp"
#include "storage/StorageManager.cpp"
#include "time/TimeUtils.cpp"
#include "upload/Uploader.cpp"
#include "network/WifiManager.cpp"

static unsigned long lastCollectTime = 0;
static unsigned long lastUploadCheckTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("ESP32 数据收集与上传程序 (ESP32_2)");
  Serial.println("========================================\n");

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

  int storedCount = getStoredDataCount();
  if (storedCount > 0) {
    Serial.print("[存储] 发现 ");
    Serial.print(storedCount);
    Serial.println(" 条未上传的数据");
  } else {
    Serial.println("[存储] 无未上传的数据");
  }

  loadStoredWiFiCredentials();

  Serial.println("[WiFi] 初始化 WiFi...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  delay(100);
  ensureConfigAP();
  beginConfigServer();

  bool wifiConnected = connectWiFi();

  if (wifiConnected) {
    syncNTPTime();
  } else {
    Serial.println("[WiFi] Wi-Fi 连接失败，自动进入配置模式");
    startConfigPortal();
  }

  lastCollectTime = 0;
  lastUploadCheckTime = 0;

  Serial.println("\n[系统] 初始化完成");
  Serial.println("[模式] 数据收集与智能上传模式：");
  Serial.println("  ✓ 每5秒收集一条数据（模拟长度 + 日期时间含时区）");
  Serial.println("  ✓ 智能上传策略：");
  Serial.println("    - 本地无待上传数据且网络正常 → 直接上传（不保存到本地）");
  Serial.println("    - 本地有待上传数据或网络不可用 → 保存到本地");
  Serial.println("    - 直接上传失败 → 自动保存到本地等待后续上传");
  Serial.println("  ✓ 只要有网络且本地有数据，持续批量上传（不受5秒间隔限制）");
  Serial.println("  ✓ 严格 FIFO 顺序：先保存的数据先上传");
  Serial.print("  ✓ 批量上传：每次上传 ");
  Serial.print(BATCH_UPLOAD_SIZE);
  Serial.println(" 条数据，提高上传效率");
  Serial.println("  ✓ 上传成功后批量删除，失败则保留数据等待重试");
  Serial.print("[配置] API Key: ");
  Serial.println(API_KEY);
  Serial.print("[配置] 传感器ID: ");
  Serial.println(ULTRASONIC_SENSOR_ID);
  Serial.println("========================================\n");
}

void loop() {
  unsigned long now = millis();
  ensureConfigAP();

  handleConfigServer();

  if (configPortalActive) {
    if (CONFIG_PORTAL_TIMEOUT_MS > 0 &&
        (now - configPortalLastActivity) > CONFIG_PORTAL_TIMEOUT_MS) {
      Serial.println("[WiFi] 配置模式超时，重启设备重试");
      delay(1000);
      ESP.restart();
    }
    delay(20);
    return;
  }

  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected && !wasConnected) {
    Serial.println("\n[网络] WiFi 已恢复连接");
    announceConfigServerAddress();
    if (!timeSynced) {
      syncNTPTime();
    }
  }

  if (!isConnected) {
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt >= 10000) {
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

  if (isConnected && !isUploading) {
    if (now - lastUploadCheckTime >= UPLOAD_CHECK_INTERVAL) {
      lastUploadCheckTime = now;
      uploadLocalData();
    }
  }

  if (now - lastCollectTime >= COLLECT_INTERVAL) {
    lastCollectTime = now;

    float simulatedDistance = generateSimulatedDistance();

    time_t currentTimestamp = getCurrentTimestamp();

    if (isConnected && !timeSynced) {
      syncNTPTime();
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

    int storedCount = getStoredDataCount();
    bool hasPendingData = (storedCount > 0);

    if (!hasPendingData && isConnected && !isUploading) {
      Serial.print(" | 本地无待上传数据，尝试直接上传...");

      if (uploadSingleData(simulatedDistance, currentTimestamp)) {
        Serial.println(" ✓ 直接上传成功（未保存到本地）");
      } else {
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

  delay(50);
  yield();
}

