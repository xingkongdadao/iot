#ifndef GPS_MODULE_H
#define GPS_MODULE_H

#include <Arduino.h>

struct GPSReading {
  double latitude = 0.0;
  double longitude = 0.0;
  double altitude = 0.0;
  double speedKmph = 0.0;
  uint8_t satellites = 0;
  String timestamp;
  bool fixValid = false;
};

// 初始化 GPS 模块（默认使用 9600 波特率）
void gpsBegin(int8_t rxPin, int8_t txPin, uint32_t baud = 9600);

// 轮询 GPS，并在获取到有效数据时填充 reading，返回 true
bool gpsPoll(GPSReading& reading);

#endif  // GPS_MODULE_H

