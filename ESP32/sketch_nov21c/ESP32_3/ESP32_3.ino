/*
 * ESP32-C3 LED + GPS Demo (IO10 + UART1)
 * --------------------------------------
 * - IO10 上的二极管保持 1 秒亮 / 0.5 秒灭的循环（非阻塞）
 * - 独立 GPS 模块负责解析串口 NMEA 信息，并通过串口输出最新坐标
 */

#include "GPSModule.h"

const int LED_PIN = 10;               // 二极管连接的 IO 口
const unsigned long ON_TIME = 1000;   // 亮灯持续 1 秒
const unsigned long OFF_TIME = 500;   // 熄灭持续 0.5 秒

const int GPS_RX_PIN = 4;             // ESP32 接收 GPS TX
const int GPS_TX_PIN = 5;             // ESP32 发送到 GPS RX（如模块只需输出，可悬空）
const uint32_t GPS_BAUD = 9600;

unsigned long lastToggle = 0;
bool ledState = false;

void printGpsReading(const GPSReading& reading) {
  Serial.print(F("[GPS] Fix "));
  Serial.print(reading.latitude, 6);
  Serial.print(F(", "));
  Serial.print(reading.longitude, 6);
  Serial.print(F(" Alt: "));
  Serial.print(reading.altitude, 1);
  Serial.print(F("m Sat: "));
  Serial.print(reading.satellites);
  Serial.print(F(" Speed: "));
  Serial.print(reading.speedKmph, 1);
  Serial.print(F("km/h Time: "));
  Serial.println(reading.timestamp);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  gpsBegin(GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
  Serial.println(F("[GPS] 初始化完成，等待定位..."));
}

void loop() {
  unsigned long interval = ledState ? ON_TIME : OFF_TIME;
  unsigned long now = millis();

  if (now - lastToggle >= interval) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastToggle = now;
  }

  GPSReading reading;
  if (gpsPoll(reading)) {
    printGpsReading(reading);
  }
}
