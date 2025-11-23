#include <Servo.h>
#include <SoftwareSerial.h>

// ESP8266 WiFi模块配置（通过软件串口连接）
// ESP8266 的 TX 接 Arduino 的 D4，RX 接 Arduino 的 D5
SoftwareSerial esp8266(4, 5);  // RX, TX (ESP8266的TX接Arduino的D4，RX接D5)

// WiFi 配置
const char* ssid = "GOGOTRANS";
const char* password = "18621260183";

// API 配置
const char* apiBaseUrl = "https://manage.gogotrans.com/api";
const char* apiKey = "ultrasonic_sensor_c2fd255e";  // 替换为你的API Key

// 超声波传感器ID（UUID格式，需要先在后台创建传感器后获取）
// 使用 char 数组代替 String 以节省内存
const char ultrasonicSensorId[] = "8ea58210-c649-11f0-afa3-da038af01e18";  // 默认值，可根据实际情况修改

// 数据上传间隔（毫秒）
const unsigned long uploadInterval = 5000;  // 5秒
unsigned long lastUploadTime = 0;

// ESP8266 状态
bool wifiConnected = false;

// 发光二极管（LED）接线说明：
//   - 红色 LED（长脚）接数字引脚 D13（经限流电阻后更安全），短脚接 GND
//   - 绿色 LED（长脚）接数字引脚 D12，短脚同样接 GND
//   - 黄色 LED（长脚）接数字引脚 D11，短脚接 GND
//   - 超声波传感器（Trig 接 D8，Echo 接 D9，VCC 接 5V，GND 接 GND）
//   - 9g Micro Servo（从左到右分别是：黄线=信号（接 D6）、红线=VCC（5V）、棕线=GND）
//   - 5461AS-1 共阳四位数码管：
//       * 段 a~g、dp 分别接 D2、D3、D4、D5、D7、D10、A0、A1（需串联限流电阻）
//       * 位选 1~4 分别接 A2、A3、A4、A5（共阳，输出 HIGH 激活）
//   - ESP8266 WiFi模块（2×4 针脚，如 ESP-01，通过软件串口连接）：
//       * 模块引脚排列（俯视天线朝上、针脚朝下）：
//           左排自上而下：GND、GPIO2、GPIO0、RXD
//           右排自上而下：VCC、RST、CH_PD（EN）、TXD
//       * 接线建议：
//           - VCC → Arduino 3.3V（必须稳定，禁止 5V）
//           - GND → Arduino GND
//           - CH_PD/EN → 3.3V（保持高电平使能模块）
//           - RST → 可用 3.3V 通过 10kΩ 上拉，或留空；需要复位时短接到 GND
//           - TXD（模块） → Arduino D4（即 SoftwareSerial RX）
//           - RXD（模块） → Arduino D5（即 SoftwareSerial TX，建议串联分压 5V→3.3V）
//           - GPIO0、GPIO2 → 日常使用可悬空或上拉到 3.3V；刷固件时需按需求接地
//       * 供电注意：
//           - ESP8266 峰值电流约 300mA，建议使用独立 3.3V 稳压模块或 AMS1117 等
//           - 所有 GND 必须共地
const int redLedPin = 13;    // 红色 LED 使用 D13（可利用板载 LED）
const int greenLedPin = 12;  // 绿色 LED 使用 D12
const int yellowLedPin = 11; // 黄色 LED 使用 D11
const int trigPin = 8;         // 超声波传感器 Trig
const int echoPin = 9;         // 超声波传感器 Echo
const int servoPin = 6;        // 9g 舵机信号引脚
const float distanceChangeThreshold = 5.0f;  // 认为“有变化”的距离差（cm）

// 数码管段位引脚（共阳，段亮=LOW）
const int segmentPins[8] = {2, 3, 4, 5, 7, 10, A0, A1};   // a,b,c,d,e,f,g,dp
const int digitPins[4] = {A2, A3, A4, A5};                // DIG1~DIG4

// 0~9、空白、减号的段编码（bit0=a，bit6=g，bit7=dp；1=亮）
const byte digitMap[12] = {
  B00111111,  // 0
  B00000110,  // 1
  B01011011,  // 2
  B01001111,  // 3
  B01100110,  // 4
  B01101101,  // 5
  B01111101,  // 6
  B00000111,  // 7
  B01111111,  // 8
  B01101111,  // 9
  B00000000,  // 空白
  B01000000   // “-”
};

int displayDigits[4] = {10, 10, 10, 10};  // 默认空白

float lastDistance = -1.0f;
Servo microServo;
int servoAngle = 90;                // 当前舵机角度（0°=最左，180°=最右）
const int servoStep = 50;           // 每次变化旋转的角度步进

void setup() {
  pinMode(redLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);
  pinMode(yellowLedPin, OUTPUT);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  for (int i = 0; i < 8; i++) {
    pinMode(segmentPins[i], OUTPUT);
    digitalWrite(segmentPins[i], HIGH);  // 共阳，默认灭
  }
  for (int i = 0; i < 4; i++) {
    pinMode(digitPins[i], OUTPUT);
    digitalWrite(digitPins[i], LOW);  // 位选默认关闭
  }
  microServo.attach(servoPin);
  microServo.write(servoAngle);
  Serial.begin(9600);
  esp8266.begin(115200);  // ESP8266 默认波特率
  delay(1000);
  
  // 初始化ESP8266 WiFi模块
  Serial.println(F("正在初始化ESP8266..."));
  if (initESP8266()) {
    Serial.println(F("ESP8266初始化成功!"));
    Serial.println(F("正在连接WiFi..."));
    if (connectWiFi()) {
      Serial.println(F("WiFi已连接!"));
      wifiConnected = true;
    } else {
      Serial.println(F("WiFi连接失败!"));
      wifiConnected = false;
    }
  } else {
    Serial.println(F("ESP8266初始化失败，请检查连接!"));
    wifiConnected = false;
  }
  
  Serial.print(F("使用超声波传感器ID: "));
  Serial.println(ultrasonicSensorId);
  
  Serial.println(F("初始化完成!"));
}

void loop() {
  refreshDisplay();

  // 检查WiFi连接（每30秒检查一次）
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) {
    lastWiFiCheck = millis();
    if (!wifiConnected || !checkWiFiConnection()) {
      Serial.println("WiFi断开，正在重连...");
      wifiConnected = connectWiFi();
      if (wifiConnected) {
        Serial.println("WiFi已重连!");
      }
    }
  }

  float distanceCm = measureDistanceCm();
  Serial.print("Distance: ");
  Serial.print(distanceCm);
  Serial.println(" cm");

  bool distanceValid = distanceCm >= 0.0f;
  bool distanceChanged = false;
  float signedDiff = 0.0f;

  updateDisplayFromDistance(distanceCm, distanceValid);

  if (distanceValid) {
    if (lastDistance >= 0.0f) {
      signedDiff = distanceCm - lastDistance;
      float absDiff = signedDiff;
      if (absDiff < 0) {
        absDiff = -absDiff;
      }
      if (absDiff >= distanceChangeThreshold) {
        distanceChanged = true;
        Serial.print("Distance changed by ");
        Serial.print(absDiff);
        Serial.println(" cm");
        updateServoAngle(signedDiff);
      }
    }
    lastDistance = distanceCm;
  } else {
    distanceChanged = true;  // 无法测得距离也视为变化
    Serial.println("Distance invalid, keeping servo angle.");
  }

  // 每5秒上传一次超声波数据
  if (millis() - lastUploadTime >= uploadInterval) {
    lastUploadTime = millis();
    if (ultrasonicSensorId[0] != '\0' && distanceValid) {
      Serial.println(F("========== 开始上传数据 =========="));
      updateUltrasonicSensor(distanceCm);
      Serial.println(F("===================================="));
    } else if (!distanceValid) {
      Serial.println(F("[上传跳过] 距离数据无效"));
    } else {
      Serial.println(F("[上传跳过] 传感器ID未设置"));
    }
  }

  if (distanceChanged) {
    // 同步闪烁：三灯一起亮 / 熄
    digitalWrite(redLedPin, HIGH);
    digitalWrite(greenLedPin, HIGH);
    digitalWrite(yellowLedPin, HIGH);
    delayWithDisplay(200);
    digitalWrite(redLedPin, LOW);
    digitalWrite(greenLedPin, LOW);
    digitalWrite(yellowLedPin, LOW);
    delayWithDisplay(200);
  } else {
    // 距离稳定：保持所有灯常亮
    digitalWrite(redLedPin, HIGH);
    digitalWrite(greenLedPin, HIGH);
    digitalWrite(yellowLedPin, HIGH);
    delayWithDisplay(200);
  }
}

float measureDistanceCm() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000UL);  // 最长等待 30ms（约 5m）
  if (duration == 0) {
    return -1.0;  // 超时或无回波
  }

  float distance = duration * 0.0343f / 2.0f;  // 声速 343 m/s
  return distance;
}

void updateServoAngle(float diff) {
  if (diff > 0) {
    servoAngle -= servoStep;  // 距离变大，向左转（角度减小）
    if (servoAngle < 0) {
      servoAngle = 0;
    }
    Serial.println("Servo turning left.");
  } else if (diff < 0) {
    servoAngle += servoStep;  // 距离变小，向右转（角度增大）
    if (servoAngle > 180) {
      servoAngle = 180;
    }
    Serial.println("Servo turning right.");
  } else {
    Serial.println("Servo stays still (no change).");
    return;
  }

  microServo.write(servoAngle);
  Serial.print("Servo angle: ");
  Serial.println(servoAngle);
}

void updateDisplayFromDistance(float distanceCm, bool valid) {
  if (!valid) {
    for (int i = 0; i < 4; i++) {
      displayDigits[i] = 11;  // “-”
    }
    return;
  }

  if (distanceCm > 9999.0f) {
    distanceCm = 9999.0f;
  } else if (distanceCm < 0.0f) {
    distanceCm = 0.0f;
  }

  int value = static_cast<int>(distanceCm);
  for (int i = 3; i >= 0; i--) {
    displayDigits[i] = value % 10;
    value /= 10;
  }

  bool seenNonZero = false;
  for (int i = 0; i < 3; i++) {
    if (!seenNonZero && displayDigits[i] == 0) {
      displayDigits[i] = 10;  // 前导零变空白
    } else {
      seenNonZero = true;
    }
  }
  if (!seenNonZero && displayDigits[3] == 0) {
    displayDigits[3] = 0;  // 数值为 0 时显示 0
  }
}

void refreshDisplay() {
  static uint8_t currentDigit = 0;
  static unsigned long lastMicros = 0;
  const unsigned long refreshInterval = 2000;  // 2ms 刷新
  unsigned long now = micros();
  if (now - lastMicros < refreshInterval) {
    return;
  }
  lastMicros = now;

  for (int i = 0; i < 4; i++) {
    digitalWrite(digitPins[i], LOW);  // 先关闭所有位
  }

  byte encoding = digitMap[displayDigits[currentDigit]];
  for (int seg = 0; seg < 8; seg++) {
    bool on = encoding & (1 << seg);
    digitalWrite(segmentPins[seg], on ? LOW : HIGH);  // 共阳：LOW 点亮
  }

  digitalWrite(digitPins[currentDigit], HIGH);  // 打开当前位
  currentDigit = (currentDigit + 1) % 4;
}

void delayWithDisplay(unsigned long durationMs) {
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    refreshDisplay();
  }
}

// 发送AT命令到ESP8266并等待响应（优化内存使用）
bool sendATCommand(const char* command, int timeout = 2000) {
  esp8266.println(command);
  char response[128] = {0};  // 固定大小的缓冲区
  int idx = 0;
  unsigned long startTime = millis();
  
  while (millis() - startTime < timeout && idx < 127) {
    refreshDisplay();  // 保持显示刷新
    if (esp8266.available()) {
      char c = esp8266.read();
      if (idx < 127) {
        response[idx++] = c;
        response[idx] = '\0';
      }
      // 检查关键响应
      if (strstr(response, "OK") || strstr(response, "ERROR") || strstr(response, "FAIL")) {
        break;
      }
    }
  }
  
  return (strstr(response, "OK") != NULL);
}

// 初始化ESP8266
bool initESP8266() {
  sendATCommand("AT", 1000);
  delay(500);
  
  if (sendATCommand("AT+RST", 3000)) {
    delay(2000);
  }
  
  return sendATCommand("AT+CWMODE=1", 2000);
}

// 连接WiFi（优化内存使用）
bool connectWiFi() {
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
  return sendATCommand(cmd, 10000);
}

// 检查WiFi连接状态
bool checkWiFiConnection() {
  char response[128] = {0};
  int idx = 0;
  esp8266.println("AT+CWJAP?");
  unsigned long startTime = millis();
  
  while (millis() - startTime < 2000 && idx < 127) {
    refreshDisplay();
    if (esp8266.available()) {
      char c = esp8266.read();
      if (idx < 127) {
        response[idx++] = c;
        response[idx] = '\0';
      }
    }
  }
  
  return (strstr(response, ssid) != NULL);
}

// 发送HTTP PATCH请求（优化内存使用）
bool sendPatchRequest(const char* endpoint, const char* jsonData) {
  if (!wifiConnected) {
    return false;
  }
  
  const char* host = "manage.gogotrans.com";
  bool useSSL = true;
  int port = useSSL ? 443 : 80;
  
  Serial.print(F("[连接] 正在连接到服务器: "));
  Serial.print(host);
  Serial.print(F(":"));
  Serial.println(port);
  
  // 尝试建立SSL连接
  char cmd[128];
  if (useSSL) {
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"SSL\",\"%s\",%d", host, port);
    Serial.println(F("[连接] 尝试SSL/HTTPS连接..."));
  } else {
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
    Serial.println(F("[连接] 尝试TCP/HTTP连接..."));
  }
  
  bool connected = sendATCommand(cmd, 10000);
  
  // 如果SSL失败，尝试普通TCP（HTTP）
  if (!connected && useSSL) {
    Serial.println(F("[连接] SSL连接失败，尝试HTTP..."));
    useSSL = false;
    port = 80;
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
    connected = sendATCommand(cmd, 5000);
  }
  
  if (connected) {
    Serial.println(F("[连接] ✓ 服务器连接成功"));
  } else {
    Serial.println(F("[连接] ✗ 服务器连接失败"));
    return false;
  }
  
  delay(500);
  
  // 构建HTTP PATCH请求（使用字符数组）
  int jsonLen = strlen(jsonData);
  char httpRequest[512];
  int len = snprintf(httpRequest, sizeof(httpRequest),
    "PATCH %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "X-API-Key: %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n"
    "\r\n"
    "%s",
    endpoint, host, apiKey, jsonLen, jsonData);
  
  if (len >= sizeof(httpRequest)) {
    Serial.println(F("[发送] ✗ 请求数据过长"));
    sendATCommand("AT+CIPCLOSE", 1000);
    return false;
  }
  
  Serial.print(F("[发送] 正在发送数据 ("));
  Serial.print(len);
  Serial.println(F(" 字节)..."));
  
  char sendCmd[32];
  snprintf(sendCmd, sizeof(sendCmd), "AT+CIPSEND=%d", len);
  
  // 检查是否进入发送模式（需要读取响应中的">"）
  esp8266.println(sendCmd);
  delay(100);
  bool readyToSend = false;
  unsigned long startTime = millis();
  while (millis() - startTime < 2000) {
    refreshDisplay();
    if (esp8266.available()) {
      char c = esp8266.read();
      if (c == '>') {
        readyToSend = true;
        break;
      }
    }
  }
  
  if (readyToSend) {
    // 发送HTTP请求
    esp8266.print(httpRequest);
    Serial.println(F("[发送] ✓ 数据已发送，等待服务器响应..."));
    
    // 等待响应（使用固定缓冲区）
    char httpResponse[512] = {0};
    int respIdx = 0;
    startTime = millis();
    while (millis() - startTime < 10000 && respIdx < 511) {
      refreshDisplay();
      if (esp8266.available()) {
        char c = esp8266.read();
        if (respIdx < 511) {
          httpResponse[respIdx++] = c;
          httpResponse[respIdx] = '\0';
        }
        if (strstr(httpResponse, "\r\n\r\n") && 
            (strstr(httpResponse, "CLOSED") || respIdx > 400)) {
          break;
        }
      }
    }
    
    // 关闭连接
    sendATCommand("AT+CIPCLOSE", 1000);
    Serial.println(F("[连接] 连接已关闭"));
    
    // 检查响应状态码
    if (strstr(httpResponse, "200 OK") || strstr(httpResponse, "204 No Content") ||
        strstr(httpResponse, "HTTP/1.1 200") || strstr(httpResponse, "HTTP/1.1 204")) {
      Serial.println(F("[响应] ✓ 服务器返回成功状态码"));
      return true;
    } else {
      Serial.print(F("[响应] ✗ 服务器响应异常"));
    }
  } else {
    Serial.println(F("[发送] ✗ 发送数据失败，无法进入发送模式"));
    sendATCommand("AT+CIPCLOSE", 1000);
  }
  
  return false;
}

// 更新超声波传感器数据（优化内存使用）
void updateUltrasonicSensor(float distanceCm) {
  if (ultrasonicSensorId[0] == '\0') {
    Serial.println(F("[上传失败] 超声波传感器ID未设置"));
    return;
  }
  
  if (!wifiConnected) {
    Serial.println(F("[上传失败] WiFi未连接"));
    return;
  }
  
  // 构建endpoint和JSON（使用字符数组）
  char endpoint[128];
  snprintf(endpoint, sizeof(endpoint), "/device/ultrasonicSensor/%s/", ultrasonicSensorId);
  
  char jsonData[64];
  snprintf(jsonData, sizeof(jsonData), "{\"distance\":%.2f}", distanceCm);
  
  Serial.print(F("[上传中] 距离: "));
  Serial.print(distanceCm, 2);
  Serial.print(F(" cm | JSON: "));
  Serial.println(jsonData);
  
  bool success = sendPatchRequest(endpoint, jsonData);
  if (success) {
    Serial.println(F("[上传成功] ✓ 数据已成功上传到服务器"));
  } else {
    Serial.println(F("[上传失败] ✗ 数据上传失败，请检查网络连接"));
  }
}
