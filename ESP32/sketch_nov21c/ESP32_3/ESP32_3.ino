/*
 * ESP32-C3 LED Blinker (IO10)
 * ---------------------------
 * 让连接在 IO10 上的二极管保持 1 秒亮、0.5 秒灭的循环。
 * 使用 millis() 进行非阻塞控制，不会阻塞主循环后续可扩展的逻辑。
 */

const int LED_PIN = 10;          // 二极管连接的 IO 口
const unsigned long ON_TIME = 1000;   // 亮灯持续 1 秒
const unsigned long OFF_TIME = 500;   // 熄灭持续 0.5 秒

unsigned long lastToggle = 0;
bool ledState = false;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void loop() {
  unsigned long interval = ledState ? ON_TIME : OFF_TIME;
  unsigned long now = millis();
  
  if (now - lastToggle >= interval) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastToggle = now;
  }

  // 这里可以继续添加其他逻辑（串口、传感器等），不会被灯控逻辑阻塞
}
