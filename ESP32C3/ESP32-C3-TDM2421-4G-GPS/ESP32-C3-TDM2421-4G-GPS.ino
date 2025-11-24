
#include <HardwareSerial.h> // 引入硬件串口库以访问 UART 外设
#include <WiFi.h> // 引入官方 WiFi 库以管理 STA/AP/多模式连接

#define simSerial                 Serial0 // 将主控与 4G/GPS 模块之间的串口实例命名为 simSerial
#define MCU_SIM_BAUDRATE          115200 // 模块串口通信波特率
#define MCU_SIM_TX_PIN            21 // ESP32-C3 对应模块 RX 的 TX 引脚
#define MCU_SIM_RX_PIN            20 // ESP32-C3 对应模块 TX 的 RX 引脚
#define MCU_SIM_EN_PIN            2 // 模块电源/使能管脚；硬件 V2 需要用 IO2
#define MCU_LED                   10 // 板载 LED 管脚
#define PHONE_NUMBER            "0..." // 发送短信的目标手机号
// Thay bằng thông tin WiFi của bạn
const char* ssid = "GOGOTRANS";        // 目标 Wi-Fi SSID
const char* password = "18621260183"; // 目标 Wi-Fi 密码


void sim_at_wait() {
    delay(500); // 留出时间等待模块返回
    while (simSerial.available()) { // 只要模块缓冲区有数据
        Serial.write(simSerial.read()); // 逐字节转发到 USB 串口，方便调试
    }
}


bool sim_at_cmd(String cmd) {
    Serial.print("Sending command: "); // 控制台提示
    Serial.println(cmd); // 打印即将发送的 AT 指令
    simSerial.println(cmd); // 将完整指令写入模块串口（带换行）
    delay(500); // 简单等待响应
    sim_at_wait(); // 读取并显示模块回复
    return true; // 简单返回 true，未做超时判断
}

bool sim_at_send(char c) {
    simSerial.write(c); // 直接写入一个字节（多用于发送 Ctrl+Z）
    return true; // 同样固定返回 true
}

void sent_sms() {
    sim_at_cmd("AT+CMGF=1"); // 设置短信文本模式
    String temp = "AT+CMGS=\""; // 组装发送号码指令头
    temp += PHONE_NUMBER; // 填入目标号码
    temp += "\""; // 补齐结尾引号
    sim_at_cmd(temp); // 发送号码指令
    sim_at_cmd("hello from TDLOGY"); // 写入短信内容
    sim_at_send(0x1A); // 发送 Ctrl+Z 告知模块完成输入
}



void get_gps_data() {
    // Bật GPS
    sim_at_cmd("AT+QGPS=1"); // 打开 GNSS 引擎
    delay(10000); // 等待模块搜星
    sim_at_cmd("AT+QGPS?"); // 查询 GPS 状态
    sim_at_cmd("AT+QGPSLOC=0"); // Nhận thông tin GPS
}

void setup() {
    pinMode(MCU_SIM_EN_PIN, OUTPUT); // 设置模块电源引脚为输出
    digitalWrite(MCU_SIM_EN_PIN, HIGH); // 拉高以打开模块
    delay(500);   // Thả PWRKEY lên cao
    Serial.begin(115200); // 初始化 USB 串口用于调试
    pinMode(MCU_LED, OUTPUT); // LED 引脚输出模式
    digitalWrite(MCU_LED, HIGH); // 点亮 LED 提示系统启动
    Serial.println("\n\n\n\n-----------------------\nSystem started!!!!"); // 打印启动提示
    delay(8000); // 留出模块上电稳定时间
    Serial.println("Starting ESP32 WiFi Test..."); // 提示即将进行 Wi-Fi 测试

    // Đặt ESP32 ở chế độ Station (kết nối với WiFi)
    WiFi.mode(WIFI_STA); // 将 ESP32 设置为 STA 模式
    WiFi.begin(ssid, password); // 发起 Wi-Fi 连接

    Serial.print("Connecting to "); // 打印连接中的 SSID
    Serial.println(ssid); // 显示目标网络

    // Chờ kết nối WiFi
    while (WiFi.status() != WL_CONNECTED) { // 循环等待 Wi-Fi 连接成功
        delay(500); // 每次查询间隔 500ms
        Serial.print("."); // 控制台打印进度点
    }

    // Khi kết nối thành công
    Serial.println(""); // 换行
    Serial.println("WiFi connected!"); // 提示连接成功
    Serial.print("IP Address: "); // 准备打印 IP
    Serial.println(WiFi.localIP());      // Hiển thị địa chỉ IP
    Serial.print("RSSI: "); // 打印信号强度标签
    Serial.println(WiFi.RSSI());         // Hiển thị cường độ tín hiệu (dBm)
    Serial.print("MAC Address: "); // 打印 MAC 标签
    Serial.println(WiFi.macAddress());   // Hiển thị địa chỉ MAC

    simSerial.begin(MCU_SIM_BAUDRATE, SERIAL_8N1, MCU_SIM_RX_PIN, MCU_SIM_TX_PIN); // 初始化模块串口
    Serial.println("Checking AT command..."); // 提示开始检测 AT
    sim_at_cmd("AT"); // 发送基本 AT 测试
    Serial.println("Getting product info..."); // 提示查询模块信息
    sim_at_cmd("ATI"); // 读取模块版本/型号
    Serial.println("Checking SIM status..."); // 提示检测 SIM
    sim_at_cmd("AT+CPIN?"); // 查询 SIM 卡状态
    Serial.println("Checking signal quality..."); // 提示检测信号
    sim_at_cmd("AT+CSQ"); // 查询信号强度
    Serial.println("Getting IMSI..."); // 提示读取 IMSI
    sim_at_cmd("AT+CIMI"); // 读取 IMSI
    //sent_sms();
    // delay(5000);
    if (WiFi.status() == WL_CONNECTED) { // 再次确认 Wi-Fi 状态
        Serial.println("Still connected to WiFi"); // 仍在线提示
        Serial.print("RSSI: "); // 打印最新 RSSI 标签
        Serial.println(WiFi.RSSI()); // 输出最新 RSSI
    } else {
        Serial.println("WiFi disconnected! Attempting to reconnect..."); // 离线提示
        WiFi.reconnect(); // 尝试重连
    }
    get_gps_data(); // 执行一次 GPS 初始化与定位

}
void loop() {
    if (Serial.available()) { // 若 USB 串口有输入
        char c = Serial.read(); // 读取单个字符
        simSerial.write(c); // 透传给模块，实现命令行交互
    }
    sim_at_cmd("AT+QGPSLOC=0");// đọc lại vị trí GPS sau 5S
    delay(5000); // 每 5 秒刷新一次定位

}
