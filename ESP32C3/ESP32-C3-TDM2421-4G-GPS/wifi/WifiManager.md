# WifiManager 使用说明

## 功能概览
- **自动联网**：上电后优先使用 NVS 中保存的 `SSID`/`Password` 连接 Wi-Fi。
- **配置门户**：无凭据或多次连接失败时，自动启动热点 `gogotrans_wifi_setup`（密码 `12345678`）和 Web 表单。
- **凭据持久化**：表单提交后写入 `Preferences`，成功联网会自动关闭热点。

## 主要接口
- `WifiManager::begin()`：在 `setup()` 调用，配置 Wi-Fi 栈并加载凭据。
- `WifiManager::ensureConnected()`：在 `loop()` 中反复调用，负责联网和失败后的自动重试。
- `WifiManager::loop()`：处理 WebServer 请求、维护配置门户超时。

## 配网流程
1. **首次使用/无凭据**  
   - 设备会广播 `gogotrans_wifi_setup` 热点，密码 `12345678`。  
   - 手机连入后访问 `http://192.168.4.1`。  
   - 输入目标路由器的 SSID/密码并提交，设备会立即尝试连接。
2. **已有凭据但连接失败**  
   - 达到 `AppConfig::WIFI_MAX_ATTEMPTS` 后会自动打开同一配置门户。  
   - 提交新凭据后，页面提示“保存成功”，热点会在连接成功后自动关闭。
3. **恢复出厂/重置凭据**  
   - 擦除 `Preferences` 中 `wifi` 命名空间（例如调用 `wifiPrefs.clear()` 或通过额外按钮逻辑），即可重新进入配置模式。

## 集成示例
```cpp
void setup() {
    Serial.begin(115200);
    WifiManager::begin();
}

void loop() {
    WifiManager::ensureConnected();
    WifiManager::loop();
    // 其余业务逻辑...
}
```

## 调试建议
- 串口日志会输出连接状态、失败原因与门户启停信息。
- 确认 `AppConfig::WIFI_ENABLED` 为 `true`，避免被其他模块强制关闭 Wi-Fi。
- 如需延长配置门户保持时间，可调整 `PORTAL_TIMEOUT_MS`。

