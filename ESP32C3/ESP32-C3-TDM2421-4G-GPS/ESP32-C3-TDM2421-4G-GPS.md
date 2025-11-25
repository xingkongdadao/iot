# ESP32-C3-TDM2421-4G-GPS Sketch Overview

`ESP32-C3-TDM2421-4G-GPS.ino` implements a geo-tracking firmware that acquires GNSS fixes from a TDM2421-based 4G/GPS module, pushes them to a REST API over Wi-Fi, and falls back to the cellular modem when Wi-Fi is unavailable. It also persists unsent fixes in NVS so that data survives reboots.

## Hardware & Compile-Time Configuration
- **UART bridge**: `simSerial` (`Serial0`) connects the ESP32-C3 to the TDM2421 module via pins `MCU_SIM_TX_PIN`/`MCU_SIM_RX_PIN` with an enable pin `MCU_SIM_EN_PIN`.
- **Wi-Fi credentials**: `ssid`, `password`, retry counts, and timeouts control STA reconnection logic.
- **Geo sensor identity**: `GEO_SENSOR_API_BASE_URL`, `GEO_SENSOR_KEY`, `GEO_SENSOR_ID`, upload interval, and exponential backoff profile.
- **Cellular APN**: `CELL_APN`, user/pass, PDP context/socket IDs, HTTP chunk sizes, and timeouts used during the fallback path.
- **Buffer sizing**: `GEO_SENSOR_BUFFER_CAPACITY` defines how many `GpsFix` entries are retained in RAM/flash.

## Data Structures
- `GpsFix`: latitude, longitude, altitude, speed, and an ISO8601 timestamp string generated from GPS data.
- Circular buffer (`geoSensorBuffer`, `geoSensorBufferStart`, `geoSensorBufferCount`): receives new fixes and drains when uploads succeed.
- `Preferences geoPrefs`: mirrors the circular buffer into flash; helper methods serialize, deserialize, and clear slots so buffered fixes survive resets.
- `ParsedUrl`: lightweight struct (host/path/port/https flag) used by the cellular HTTP client.

## Wi-Fi Management (`ensureWifiConnected`)
1. Configures STA-only mode, disables persistence/sleep, and enables auto-reconnect.
2. Attempts connection up to `WIFI_MAX_ATTEMPTS` with `WiFi.waitForConnectResult`.
3. Prints diagnostics (IP, RSSI, MAC) on success and throttles retries on failure.
4. Called during `setup()` and every loop iteration to keep STA connectivity alive.

## GPS Acquisition Helpers
- `sim_at_cmd*` helpers wrap AT commands sent to the modem UART and print responses.
- `get_gps_data` performs the initial `AT+QGPS=1` enabling, waits for TTFF, and queries status/location.
- `fetchGpsFix` executes `AT+QGPSLOC=0`, captures the raw response, and delegates to `parseGpsResponse`.
- `parseGpsResponse` extracts the comma-separated fields, converts latitude/longitude from NMEA to decimal (`convertNmeaToDecimal`), and builds an ISO timestamp via `buildIso8601UtcFromGps`.

## Geo Sensor Payload & Buffering
- `buildGeoSensorPayload` converts a `GpsFix` into the JSON body expected by the `/device/geoSensor/{id}/` endpoint.
- `geoSensorBufferEnqueue/DropOldest/Peek` maintain the circular queue with persistence hooks (`persistGeoSensorSlot`, `persistGeoSensorMetadata`).
- `geoSensorRecordUploadFailure/Success` implement an exponential backoff strategy so repeated failures delay future attempts.
- `flushGeoSensorBuffer` uploads buffered entries in FIFO order until either the queue is empty or the current attempt fails (which re-triggers backoff).

## Wi-Fi Upload Path (`uploadGeoSensorViaWifi`)
1. Verifies Wi-Fi status, configures an `HTTPClient`, and chooses TLS (`WiFiClientSecure`) or plain TCP (`WiFiClient`) depending on the URL scheme.
2. Adds headers (`Content-Type`, `X-API-Key`, `Connection`), sends a `PATCH` request, and logs the HTTP status and body.
3. Returns true for any 2xx response.

## Cellular Fallback
- `ensureCellularReady` sends `AT`, configures APN via `AT+QICSGP`, and activates the PDP context (`AT+QIACT`).
- `cellularOpenSocket`, `cellularCloseSocket`, and `cellularSendRequest` manage a TCP socket using `AT+QIOPEN`, `AT+QISEND`, and URCs such as `+QIURC: "closed"`/`"recv"`.
- `readCellularHttpResponse` loops with `AT+QIRD` to pull modem buffers in chunks and reconstructs the HTTP payload.
- `uploadGeoSensorViaCellular` builds the same JSON payload, crafts manual HTTP headers (PATCH), and judges success based on the parsed status line.

## Geo Sensor Scheduler (`handleGeoSensorUpdate`)
1. Runs every loop but throttles to `GEO_SENSOR_UPLOAD_INTERVAL_MS`.
2. Obtains a fresh fix; if backoff is active, queues immediately.
3. If there are existing buffered entries, enqueues the new fix and calls `flushGeoSensorBuffer` to keep ordering.
4. Attempts immediate upload when the buffer is empty, falling back to enqueue+backoff if the live upload fails.

## Application Lifecycle
- `setup()`:
  - Powers the modem, configures LED/serial ports, and prints boot diagnostics.
  - Restores buffered fixes from NVS, tries Wi-Fi, runs basic modem/ SIM/ signal checks, and performs initial GPS enabling.
  - Marks the TLS client as insecure for dev servers.
- `loop()`:
  - Bridges USB serial input to the modem for interactive debugging.
  - Keeps Wi-Fi connected, drains buffered fixes, and schedules new GPS uploads via `handleGeoSensorUpdate`.

## Operational Notes
- **Security**: `geoSecureClient.setInsecure()` skips TLS validation—acceptable for LAN testing but should be replaced with a proper root certificate in production.
- **APN/Server Configuration**: Update `CELL_APN`, API base URL, sensor ID, and API key before deploying to a different environment.
- **Power sequencing**: `MCU_SIM_EN_PIN` must align with the TDM2421 hardware revision (V2 uses GPIO2 as documented in the file comments).
- **Debugging**: Serial output mirrors every AT interaction, making it easy to trace failures in GPS acquisition, Wi-Fi, or cellular transport.

Use this document as a quick reference when onboarding contributors, reviewing telemetry flows, or porting the sketch to similar hardware.

