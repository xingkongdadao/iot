#include "TimeUtils.h"

#include <WiFi.h>
#include <time.h>

#include "../config/Config.h"

bool timeSynced = false;

void syncNTPTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[时间] WiFi 未连接，无法同步时间");
    return;
  }

  Serial.println("[时间] 正在同步 NTP 时间...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "time.nist.gov", "time.google.com");

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

time_t getCurrentTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000) {
    return 0;
  }
  return now;
}

String formatDateTime(time_t timestamp) {
  if (timestamp == 0) {
    return "";
  }

  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);

  int offsetHours = GMT_OFFSET_SEC / 3600;
  int offsetMinutes = abs((GMT_OFFSET_SEC % 3600) / 60);

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

