#pragma once

// 全局配置常量，供多个模块共享

#include <Arduino.h>

inline constexpr char DEFAULT_WIFI_SSID[] = "GOGOTRANS";
inline constexpr char DEFAULT_WIFI_PASSWORD[] = "186212601830";

inline constexpr char WIFI_PREF_NAMESPACE[] = "wifi";
inline constexpr char WIFI_PREF_SSID_KEY[] = "ssid";
inline constexpr char WIFI_PREF_PASS_KEY[] = "pass";

inline constexpr char CONFIG_AP_SSID[] = "ESP32S3_Config";
inline constexpr char CONFIG_AP_PASSWORD[] = "12345678";
inline constexpr unsigned long CONFIG_PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

inline constexpr char API_BASE_URL[] = "http://192.168.100.192:8001/api";
// inline constexpr char API_BASE_URL[] = "https://manage.gogotrans.com/api";
inline constexpr char API_KEY[] = "mcu_8312592b29fd4c68a0e01336cf26f438";
inline constexpr char ULTRASONIC_SENSOR_ID[] = "8ea58210-c649-11f0-afa3-da038af01e18";

inline constexpr char NTP_SERVER[] = "pool.ntp.org";
inline constexpr long GMT_OFFSET_SEC = 7 * 3600;
inline constexpr int DAYLIGHT_OFFSET_SEC = 0;

inline constexpr unsigned long COLLECT_INTERVAL = 5000;
inline constexpr unsigned long UPLOAD_CHECK_INTERVAL = 500;
inline constexpr int BATCH_UPLOAD_SIZE = 50;

inline constexpr char DATA_FILE_PATH[] = "/sensor_data.json";
inline constexpr size_t MIN_FREE_HEAP = 20000;
inline constexpr size_t JSON_DOC_SIZE = 1048576;

