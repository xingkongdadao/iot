# ESP32-C3 + Quectel EG800K Demo

完整示例展示 ESP32-C3 经 UART 控制 EG800K（TDM2421 开发板），实现：

- 4G 注册与 PDP 激活（APN 可配置）
- HTTP POST 上传（AT+QHTTP*）
- MQTT 连接与遥测发布（AT+QMT*）
- GNSS（GPS/GLONASS/BeiDou）定位并解析
- 非阻塞 LED 心跳指示

> ⚠️ 例程基于 Arduino Core for ESP32，需提前安装 `ArduinoJson`（可选，用于扩展）及 `TinyGSM` **无需**，示例中所有 AT 交互都在 `EG800KClient` 模块里完成。

## 推荐接线（UART1）

| ESP32-C3 | EG800K (TDM2421) |
|----------|------------------|
| GPIO21 (TXD1) | RXD |
| GPIO20 (RXD1) | TXD |
| GND | GND |
| 5V | VIN（板载 DC-DC 支持 5–12V） |

- GPS 需外接有源天线（IPEX → SMA）并置于室外。
- 如需通过 ESP32 控电源，可接 `PWRKEY` 到 MOS 管或 GPIO（本例默认模块已上电且自动开机）。

## 目录结构

- `EG800K_Demo.ino`：主循环，调度 HTTP/MQTT/GPS。
- `EG800KClient.[h|cpp]`：封装 AT 指令交互、网络与 GNSS 解析。

## 快速上手

1. **克隆/复制代码** 到 Arduino 项目文件夹。
2. **修改配置**（`EG800K_Demo.ino`顶部）：
   - `kApn`、`kApnUser`、`kApnPass`
   - `kHttpUrl`
   - `kMqttConfig`（服务器、topic 等）
3. **硬件连接** 按上表完成 UART、供电、天线。
4. **上传固件** 到 ESP32-C3，串口监视器设 115200 bps。
5. **观察串口输出**：
   - `[MODEM] Ready`：AT 同步成功
   - `[MODEM] Network attached`：PDP 激活完成
   - `[HTTP] POST OK`：HTTP 上传成功
   - `[MQTT] Publish OK`：MQTT 遥测成功
   - `[GPS] Fix ...`：输出实时经纬度/高度/速度

## 代码要点

- **AT 同步与调试**：所有响应会透传到 USB 串口，方便定位。
- **HTTP 上传**：示例向 `https://httpbin.org/post` 发送 JSON，可替换为实际 REST API。
- **MQTT**：默认连接 `broker.emqx.io`，周期性发布 GPS 数据。
- **GNSS**：调用 `AT+QGPSLOC=2` 获取综合定位信息，并转换成十进制度。
- **非阻塞心跳**：LED（GPIO10）以 0.8s/0.4s 占空比闪烁，确认主循环正常。

## 常见问题

- **串口无响应**：确认 TDM2421 已上电且波特率为 115200；如使用 USB 供电需保证 ≥5V/1A。
- **`Network attach failed`**：APN/卡状态异常，或天线未接；可先单独测试 `AT+QIACT?`。
- **无法定位**：首次冷启动需 2–5 分钟，请确保 GPS 天线视野开阔；可尝试 `AT+QGPS=1` 后等待。
- **MQTT 断线**：蜂窝网络存在延迟，建议在产线环境加入重连策略（示例中可扩展 `mqttConnect`）。

欢迎根据自身业务扩展：如使用 `AT+QHTTPGET`、`AT+QFUPL` 上传固件，或加入 `LittleFS` 本地缓存等。若需进一步集成到现有 `ESP32_2/3` 工程，可共享 `EG800KClient` 模块。 ***

