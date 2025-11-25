#include "WifiManager.h"

#include <WiFi.h>
#include <Preferences.h>

#include "../config/Config.h"

namespace {
Preferences wifiPrefs;
String activeSsid = DEFAULT_WIFI_SSID;
String activePassword = DEFAULT_WIFI_PASSWORD;
bool storedCredentialsAvailable = false;
}  // namespace

WebServer configServer(80);
bool configPortalActive = false;
unsigned long configPortalStartTime = 0;
unsigned long configPortalLastActivity = 0;

const String& getActiveSsid() {
  return activeSsid;
}

const String& getActivePassword() {
  return activePassword;
}

bool hasStoredCredentials() {
  return storedCredentialsAvailable;
}

void loadStoredWiFiCredentials() {
  storedCredentialsAvailable = false;
  if (wifiPrefs.begin(WIFI_PREF_NAMESPACE, true)) {
    String storedSsid = wifiPrefs.getString(WIFI_PREF_SSID_KEY, "");
    String storedPass = wifiPrefs.getString(WIFI_PREF_PASS_KEY, "");
    wifiPrefs.end();

    storedSsid.trim();
    storedPass.trim();

    if (storedSsid.length() > 0) {
      activeSsid = storedSsid;
      activePassword = storedPass;
      storedCredentialsAvailable = true;
      Serial.println("[WiFi] 已加载保存的 Wi-Fi 配置信息");
      return;
    }
  }

  activeSsid = DEFAULT_WIFI_SSID;
  activePassword = DEFAULT_WIFI_PASSWORD;
  Serial.println("[WiFi] 使用默认的 Wi-Fi 配置信息");
}

bool persistWiFiCredentials(const String& newSsid, const String& newPassword) {
  if (!wifiPrefs.begin(WIFI_PREF_NAMESPACE, false)) {
    return false;
  }
  bool ok = wifiPrefs.putString(WIFI_PREF_SSID_KEY, newSsid) &&
            wifiPrefs.putString(WIFI_PREF_PASS_KEY, newPassword);
  wifiPrefs.end();
  if (ok) {
    activeSsid = newSsid;
    activePassword = newPassword;
    storedCredentialsAvailable = true;
  }
  return ok;
}

void sendConfigPortalPage(const String& message) {
  String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>ESP32 Wi-Fi 设置</title>"
      "<style>body{font-family:Arial;background:#f5f5f5;margin:0;padding:0;color:#333;}"
      ".container{max-width:420px;margin:40px auto;padding:24px;background:#fff;border-radius:8px;"
      "box-shadow:0 4px 12px rgba(0,0,0,0.08);}input,button{width:100%;padding:12px;margin:8px 0;"
      "border:1px solid #ccc;border-radius:6px;}button{background:#0070f3;color:#fff;border:none;"
      "font-size:16px;cursor:pointer;}button:hover{background:#005ad1;}label{font-weight:600;}"
      ".msg{margin-top:12px;color:#d9534f;font-weight:600;text-align:center;}"
      "</style></head><body><div class='container'><h2>配置 Wi-Fi</h2>"
      "<form method='POST' action='/save'><label>Wi-Fi 名称 (SSID)</label>"
      "<input name='ssid' placeholder='如：MyHomeWiFi' required>"
      "<label>Wi-Fi 密码</label><input name='password' type='password'"
      "placeholder='至少8位（如有）'>"
      "<button type='submit'>保存并重启</button></form>");
  if (message.length() > 0) {
    html += "<div class='msg'>" + message + "</div>";
  }
  html += F("<p style='font-size:12px;color:#777;margin-top:16px;'>保存后设备将自动重启，"
            "并尝试连接到新的 Wi-Fi。</p></div></body></html>");
  configServer.send(200, "text/html", html);
}

void handleConfigPortalRoot() {
  configPortalLastActivity = millis();
  sendConfigPortalPage();
}

void handleConfigPortalSave() {
  configPortalLastActivity = millis();
  if (!configServer.hasArg("ssid")) {
    sendConfigPortalPage("请填写 Wi-Fi 名称");
    return;
  }

  String newSsid = configServer.arg("ssid");
  String newPassword = configServer.arg("password");
  newSsid.trim();
  newPassword.trim();

  if (newSsid.isEmpty()) {
    sendConfigPortalPage("Wi-Fi 名称不能为空");
    return;
  }

  if (!persistWiFiCredentials(newSsid, newPassword)) {
    sendConfigPortalPage("保存失败，请重试");
    return;
  }

  configServer.send(200, "text/plain",
                    "Wi-Fi 配置已保存，设备即将重启并连接到新网络。");
  Serial.println("[WiFi] 已保存新的 Wi-Fi 配置，准备重启...");
  delay(1500);
  ESP.restart();
}

void handleConfigPortalNotFound() {
  configPortalLastActivity = millis();
  configServer.sendHeader("Location", "/", true);
  configServer.send(302, "text/plain", "");
}

void startConfigPortal() {
  if (configPortalActive) {
    return;
  }
  configPortalActive = true;
  configPortalStartTime = millis();
  configPortalLastActivity = configPortalStartTime;

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);

  bool apStarted = WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();

  configServer.on("/", HTTP_GET, handleConfigPortalRoot);
  configServer.on("/save", HTTP_POST, handleConfigPortalSave);
  configServer.onNotFound(handleConfigPortalNotFound);
  configServer.begin();

  Serial.println("\n[WiFi] 进入配置模式");
  if (apStarted) {
    Serial.print("[WiFi] 配置热点: ");
    Serial.print(CONFIG_AP_SSID);
    Serial.print(" / 密码: ");
    Serial.println(CONFIG_AP_PASSWORD);
    Serial.print("[WiFi] 使用手机连接后，访问 http://");
    Serial.print(apIP.toString());
    Serial.println(" 进行配置");
  } else {
    Serial.println("[WiFi] 启动配置热点失败");
  }
}

bool connectWiFi() {
  Serial.print("[WiFi] 正在连接: ");
  Serial.println(activeSsid);
  if (storedCredentialsAvailable) {
    Serial.println("[WiFi] 使用已保存的网络配置");
  }

  WiFi.begin(activeSsid.c_str(), activePassword.c_str());

  int attempts = 0;
  const int maxAttempts = 20;

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print(".");
    yield();
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ✓ 连接成功! IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] 信号强度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return true;
  }

  Serial.println("[WiFi] ✗ 连接失败，将在 loop 中继续尝试或进入配置模式");
  return false;
}

