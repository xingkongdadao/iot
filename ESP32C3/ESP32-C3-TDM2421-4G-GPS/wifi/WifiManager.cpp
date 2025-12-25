#include "WifiManager.h"

#include "../config/AppConfig.h"
#include <Preferences.h>
#include <WebServer.h>

namespace {

unsigned long wifiNextRetryAt = 0;
Preferences wifiPrefs;
WebServer portalServer(80);
bool portalRunning = false;
bool credentialsAvailable = false;
bool portalRoutesConfigured = false;
bool configApStarted = false;
String configuredSsid;
String configuredPassword;
unsigned long portalLastActivity = 0;
unsigned long lastConfigApAttempt = 0;

constexpr char PREF_NAMESPACE[] = "wifi";
constexpr char PREF_SSID_KEY[] = "ssid";
constexpr char PREF_PASSWORD_KEY[] = "pass";
constexpr char AP_SSID[] = "gogotrans_wifi_setup";
constexpr char AP_PASSWORD[] = "12345678";
constexpr uint32_t CONFIG_AP_RETRY_INTERVAL_MS = 5000;

const char* wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:
            return "IDLE";
        case WL_NO_SSID_AVAIL:
            return "SSID_UNAVAILABLE";
        case WL_SCAN_COMPLETED:
            return "SCAN_COMPLETED";
        case WL_CONNECTED:
            return "CONNECTED";
        case WL_CONNECT_FAILED:
            return "CONNECT_FAILED";
        case WL_CONNECTION_LOST:
            return "CONNECTION_LOST";
        case WL_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}

void configureWifiStack() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
}

void announcePortalAccess() {
    Serial.printf("Config portal SSID '%s' (password: %s)\n", AP_SSID, AP_PASSWORD);
    Serial.print("Connect and open http://");
    Serial.print(WiFi.softAPIP());
    Serial.println(" to update Wi-Fi.");
}

void ensureConfigAP() {
    wifi_mode_t mode = WiFi.getMode();
    bool apModeActive = (mode == WIFI_AP || mode == WIFI_AP_STA);

    if (configApStarted && apModeActive) {
        return;
    }

    if (!apModeActive) {
        configApStarted = false;
        WiFi.mode(WIFI_AP_STA);
    }

    unsigned long now = millis();
    if (!configApStarted && (now - lastConfigApAttempt) < CONFIG_AP_RETRY_INTERVAL_MS) {
        return;
    }
    lastConfigApAttempt = now;

    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        Serial.println("Failed to start config AP, will retry shortly.");
        configApStarted = false;
        return;
    }

    configApStarted = true;
    Serial.print("Config AP active. IP: ");
    Serial.println(WiFi.softAPIP());
    announcePortalAccess();
}

void loadStoredCredentials() {
    wifiPrefs.begin(PREF_NAMESPACE, true);
    configuredSsid = wifiPrefs.getString(PREF_SSID_KEY, AppConfig::WIFI_SSID);
    configuredPassword = wifiPrefs.getString(PREF_PASSWORD_KEY, AppConfig::WIFI_PASSWORD);
    wifiPrefs.end();
    credentialsAvailable = configuredSsid.length() > 0;
}

void persistCredentials(const String& ssid, const String& password) {
    wifiPrefs.begin(PREF_NAMESPACE, false);
    wifiPrefs.putString(PREF_SSID_KEY, ssid);
    wifiPrefs.putString(PREF_PASSWORD_KEY, password);
    wifiPrefs.end();
    configuredSsid = ssid;
    configuredPassword = password;
    credentialsAvailable = true;
}

String htmlPage(const String& message) {
    String page = "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
                  "<title>ESP32 Wi-Fi Setup</title>"
                  "<style>body{font-family:-apple-system,system-ui;padding:16px;background:#f6f8fa;}"
                  ".card{max-width:420px;margin:auto;background:#fff;padding:24px;border-radius:12px;"
                  "box-shadow:0 10px 30px rgba(0,0,0,0.08);}label{display:block;margin-top:12px;font-weight:600;}"
                  "input{width:100%;padding:10px;margin-top:6px;border-radius:6px;border:1px solid #d0d7de;}"
                  "button{margin-top:18px;width:100%;padding:12px;border:none;border-radius:6px;background:#0070f3;"
                  "color:#fff;font-size:16px;font-weight:600;cursor:pointer;}p{margin:0;}</style></head><body>"
                  "<div class='card'><h2>Wi-Fi 配置</h2>";
    if (!message.isEmpty()) {
        page += "<p style='color:#0969da;margin-bottom:12px;'>" + message + "</p>";
    }
    if (WiFi.status() == WL_CONNECTED) {
        page += "<p style='color:#28a745;'>已连接网络，IP: " + WiFi.localIP().toString() + "</p>";
    } else {
        page += "<p style='color:#d73a49;'>当前未连接到路由器。</p>";
    }
    page += "<p style='margin-top:8px;color:#57606a;'>配置热点: <strong>" + String(AP_SSID) +
            "</strong><br/>连接后访问 <strong>http://" + WiFi.softAPIP().toString() + "</strong></p>";
    page += "<form method='POST' action='/configure'>"
            "<label>Wi-Fi 名称 (SSID)</label>"
            "<input name='ssid' value='" + configuredSsid + "' required />"
            "<label>Wi-Fi 密码 (可留空，表示无密码)</label>"
            "<input name='password' type='password' value='" + configuredPassword + "' />"
            "<button type='submit'>保存并连接</button>"
            "</form>"
            "<p style='margin-top:16px;color:#57606a;'>保存后设备会自动尝试连接新的 Wi-Fi。</p>"
            "</div></body></html>";
    return page;
}

void sendPortalPage(const String& message = "") {
    portalLastActivity = millis();
    portalServer.send(200, "text/html", htmlPage(message));
}

void handleConfigSubmit() {
    const String ssid = portalServer.arg("ssid");
    const String password = portalServer.arg("password");
    if (ssid.isEmpty()) {
        sendPortalPage("SSID 不能为空。");
        return;
    }

    persistCredentials(ssid, password);
    sendPortalPage("保存成功，正在尝试连接...");
    portalLastActivity = millis();
    wifiNextRetryAt = 0;
}

void configurePortalRoutes() {
    if (portalRoutesConfigured) {
        return;
    }
    portalServer.on("/", HTTP_GET, []() {
        sendPortalPage();
    });
    portalServer.on("/configure", HTTP_POST, handleConfigSubmit);
    portalServer.onNotFound([]() {
        portalServer.send(404, "text/plain", "Not found");
    });
    portalRoutesConfigured = true;
}

void startConfigPortal() {
    ensureConfigAP();
    configurePortalRoutes();
    if (portalRunning) {
        return;
    }
    Serial.println("Ensuring Wi-Fi setup portal is running...");
    portalServer.begin();
    portalRunning = true;
    portalLastActivity = millis();
    announcePortalAccess();
}

void handlePortalLoop() {
    if (!portalRunning) {
        return;
    }
    ensureConfigAP();
    portalServer.handleClient();
}

}  // namespace

namespace WifiManager {

void begin() {
    configureWifiStack();
    loadStoredCredentials();
    startConfigPortal();
}

bool ensureConnected() {
    if (!AppConfig::WIFI_ENABLED) {
        return false;
    }
    if (!credentialsAvailable) {
        startConfigPortal();
        return false;
    }
    startConfigPortal();
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    if (wifiNextRetryAt != 0 && millis() < wifiNextRetryAt) {
        return false;
    }

    configureWifiStack();

    // Scan networks before attempting to connect, to debug visibility and signal
    Serial.println("Scanning WiFi networks before connect...");
    int networkCount = WiFi.scanNetworks();
    Serial.printf("Found %d networks\n", networkCount);
    bool configuredSsidFound = false;
    for (int i = 0; i < networkCount; ++i) {
        String ssid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        wifi_auth_mode_t enc = WiFi.encryptionType(i);
        Serial.printf("%2d: SSID='%s', RSSI=%d dBm, ENC=%d\n", i, ssid.c_str(), rssi, static_cast<int>(enc));
        if (ssid == configuredSsid) {
            configuredSsidFound = true;
        }
    }
    if (!configuredSsidFound) {
        Serial.printf("Configured SSID '%s' not found in scan results.\n", configuredSsid.c_str());
    } else {
        Serial.printf("Configured SSID '%s' detected in scan results.\n", configuredSsid.c_str());
    }

    for (uint8_t attempt = 1; attempt <= AppConfig::WIFI_MAX_ATTEMPTS; ++attempt) {
        Serial.printf("WiFi connect attempt %u/%u\n", attempt, AppConfig::WIFI_MAX_ATTEMPTS);
        WiFi.disconnect(true, true);
        delay(200);
        if (configuredPassword.length() == 0) {
            Serial.printf("Connecting to open network SSID: %s\n", configuredSsid.c_str());
            WiFi.begin(configuredSsid.c_str());
        } else {
            WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
        }
        wl_status_t result =
            static_cast<wl_status_t>(WiFi.waitForConnectResult(AppConfig::WIFI_CONNECT_TIMEOUT_MS));
        if (result == WL_CONNECTED) {
            Serial.println("WiFi connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("RSSI: ");
            Serial.println(WiFi.RSSI());
            Serial.print("MAC Address: ");
            Serial.println(WiFi.macAddress());
            wifiNextRetryAt = 0;
            return true;
        }
        Serial.printf("WiFi connect failed -> status: %s (%d)\n", wifiStatusToString(result), result);
        delay(1000);
    }

    Serial.println("WiFi connection timeout after max attempts");
    wifiNextRetryAt = millis() + AppConfig::WIFI_RETRY_COOLDOWN_MS;
    startConfigPortal();
    return false;
}

void loop() {
    startConfigPortal();
    handlePortalLoop();
    if (portalRunning && credentialsAvailable && WiFi.status() != WL_CONNECTED) {
        ensureConnected();
    }
}

}  // namespace WifiManager

