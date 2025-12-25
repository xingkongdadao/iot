#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

namespace AppConfig {

inline HardwareSerial& modemSerial() {
    return Serial0;
}

inline constexpr uint32_t MCU_SIM_BAUDRATE = 115200;
inline constexpr uint8_t MCU_SIM_TX_PIN = 21;
inline constexpr uint8_t MCU_SIM_RX_PIN = 20;
inline constexpr uint8_t MCU_SIM_EN_PIN = 2;
inline constexpr uint8_t MCU_LED = 10;

inline constexpr char PHONE_NUMBER[] = "0...";

inline constexpr uint32_t GEO_SENSOR_UPLOAD_INTERVAL_MS = 300000UL;
inline constexpr char GEO_SENSOR_API_BASE_URL[] = "https://manage.gogotrans.com/api";
inline constexpr char GEO_SENSOR_KEY[] = "mcu_0fda5a6b27214e1eb30fe7fe2c5d4f69";
inline constexpr char GEO_SENSOR_ID[] = "4ccd94bc-c947-11f0-9ea2-12d3851b737f";

inline constexpr char WIFI_SSID[] = "米奇";
inline constexpr char WIFI_PASSWORD[] = "19963209891";
inline constexpr bool WIFI_ENABLED = true;
inline constexpr uint8_t WIFI_MAX_ATTEMPTS = 5;
inline constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
inline constexpr uint32_t WIFI_RETRY_COOLDOWN_MS = 60000;

inline constexpr unsigned long GEO_SENSOR_BACKOFF_DELAYS_MS[] = {5000UL, 60000UL, 300000UL};
inline constexpr size_t GEO_SENSOR_BACKOFF_STAGE_COUNT =
    sizeof(GEO_SENSOR_BACKOFF_DELAYS_MS) / sizeof(GEO_SENSOR_BACKOFF_DELAYS_MS[0]);

inline constexpr char CELL_APN[] = "CMNET";
inline constexpr char CELL_APN_USER[] = "";
inline constexpr char CELL_APN_PASS[] = "";
inline constexpr uint8_t CELL_CONTEXT_ID = 1;
inline constexpr uint8_t CELL_SOCKET_ID = 0;
inline constexpr uint32_t CELL_ATTACH_TIMEOUT_MS = 60000;
inline constexpr uint32_t CELL_SIM_READY_TIMEOUT_MS = 20000;
inline constexpr uint32_t CELL_REG_CHECK_INTERVAL_MS = 2000;
inline constexpr uint32_t CELL_READY_REFRESH_MS = 300000;
inline constexpr uint32_t CELL_SOCKET_OP_TIMEOUT_MS = 20000;
inline constexpr uint16_t CELL_HTTP_READ_CHUNK = 512;

inline constexpr uint16_t GEO_SENSOR_BUFFER_CAPACITY = 512;

}  // namespace AppConfig

