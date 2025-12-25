
#include <Arduino.h>
#include <WiFi.h>

#include "config/AppConfig.h"
#include "gps/GpsService.h"
#include "modem/ModemCommands.h"
#include "net/GeoUploader.h"
#include "wifi/WifiManager.h"

namespace {

// 初始化调制解调器的自检流程，确保基本 AT 命令可用
void initializeModemDiagnostics() {
  Serial.println("Checking AT command...");
  sim_at_cmd("AT");
  Serial.println("Getting product info...");
  sim_at_cmd("ATI");
  Serial.println("Checking SIM status...");
  sim_at_cmd("AT+CPIN?");
  Serial.println("Checking signal quality...");
  sim_at_cmd("AT+CSQ");
  Serial.println("Getting IMSI...");
  sim_at_cmd("AT+CIMI");
}

// 将 USB 串口输入透传到模组串口，方便直接发送 AT 指令
void forwardUsbToModem() {
  if (Serial.available()) {
    AppConfig::modemSerial().write(Serial.read());
  }
}

}  // namespace

void setup() {
  // 先给 SIM 模组供电，并点亮板载 LED，提示系统启动
  pinMode(AppConfig::MCU_SIM_EN_PIN, OUTPUT);
  digitalWrite(AppConfig::MCU_SIM_EN_PIN, HIGH);
  delay(500);

  Serial.begin(115200);
  pinMode(AppConfig::MCU_LED, OUTPUT);
  digitalWrite(AppConfig::MCU_LED, HIGH);
  Serial.println("\n\n\n\n-----------------------\nSystem started!!!!");
  delay(8000);

  // 初始化上传组件并尝试连接 Wi-Fi
  GeoUploader::init();
  WifiManager::begin();
  Serial.printf("Connecting to %s\n", AppConfig::WIFI_SSID);
  if (!WifiManager::ensureConnected()) {
    Serial.println("WiFi unavailable, will retry in loop");
  }

  // 配置与蜂窝模组的串口并执行自检，再预热 GPS
  AppConfig::modemSerial().begin(
    AppConfig::MCU_SIM_BAUDRATE, SERIAL_8N1, AppConfig::MCU_SIM_RX_PIN, AppConfig::MCU_SIM_TX_PIN);
  initializeModemDiagnostics();
  WifiManager::ensureConnected();
  GpsService::warmup();
}

void loop() {
  // 主循环中保持串口转发、网络连通以及上传逻辑
  forwardUsbToModem();
  WifiManager::ensureConnected();
  WifiManager::loop();
  GeoUploader::flushBuffer();
  GeoUploader::handleUpdate();
}
