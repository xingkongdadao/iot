#include "GPSModule.h"

#include <TinyGPSPlus.h>

namespace {

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);  // ESP32-C3 的 UART1

String buildTimestamp() {
  if (!gps.date.isValid() || !gps.time.isValid()) {
    return "";
  }

  char buffer[25];
  snprintf(buffer,
           sizeof(buffer),
           "%04d-%02d-%02dT%02d:%02d:%02dZ",
           gps.date.year(),
           gps.date.month(),
           gps.date.day(),
           gps.time.hour(),
           gps.time.minute(),
           gps.time.second());
  return String(buffer);
}

}  // namespace

void gpsBegin(int8_t rxPin, int8_t txPin, uint32_t baud) {
  gpsSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
}

bool gpsPoll(GPSReading& reading) {
  bool updated = false;

  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (gps.encode(c)) {
      updated = true;
    }
  }

  if (!updated || !gps.location.isUpdated()) {
    return false;
  }

  reading.latitude = gps.location.lat();
  reading.longitude = gps.location.lng();
  reading.altitude = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  reading.speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  reading.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
  reading.timestamp = buildTimestamp();
  reading.fixValid = gps.location.isValid();

  return reading.fixValid;
}

