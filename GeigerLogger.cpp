#include <M5AtomS3.h>  // AtomS3専用ライブラリを使用
#include <WiFi.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <driver/gpio.h>

// ======================================================
// Wi-Fi設定
// ======================================================
const char* WIFI_SSID     = "<SSID>";
const char* WIFI_PASSWORD = "<PASSWORD>";

// ======================================================
// ThingSpeak設定
// ======================================================
const char* THINGSPEAK_WRITE_API_KEY = "<API_KEY>";
const char* THINGSPEAK_URL = "https://api.thingspeak.com/update";

// ======================================================
// ハードウェアピン設定
// ======================================================
static constexpr int GEIGER_PIN = 2;   // GPIO2 (PORT A)

// ======================================================
// 測定設定
// ======================================================
static constexpr uint32_t LOG_PERIOD_MS = 60000;
static constexpr uint32_t SERIAL_PERIOD_MS = 5000;
static constexpr uint32_t MIN_PULSE_INTERVAL_US = 500;

static constexpr float CPM_PER_USV_H = 151.0f;

// ======================================================
// Geigerカウント用・LED制御用グローバル変数
// ======================================================
volatile uint32_t pulseCount = 0;
volatile uint32_t lastPulseUs = 0;
volatile bool pulseDetected = false; // パルス検知をloopに伝えるフラグ
portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t windowStartMs = 0;
uint32_t lastSerialMs = 0;

// ======================================================
// LED制御関数
// ======================================================
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  // 24bitカラーコード（0xRRGGBB）を生成
  uint32_t color = (r << 16) | (g << 8) | b;
  
  // AtomS3.disオブジェクトを使用
  AtomS3.dis.drawpix(color);
  AtomS3.update(); // 状態を更新してLEDに反映
}

void blinkLed(uint8_t r, uint8_t g, uint8_t b, int count, int delayMs) {
  for (int i = 0; i < count; i++) {
    setLedColor(r, g, b);
    delay(delayMs);
    setLedColor(0, 0, 0); // 消灯
    delay(delayMs);
  }
}

// ======================================================
// 割り込みハンドラ & カウント取得
// ======================================================
void IRAM_ATTR onGeigerPulse() {
  uint32_t nowUs = micros();
  portENTER_CRITICAL_ISR(&pulseMux);
  if ((uint32_t)(nowUs - lastPulseUs) >= MIN_PULSE_INTERVAL_US) {
    pulseCount++;
    lastPulseUs = nowUs;
    pulseDetected = true; // ISR内ではフラグを立てるのみ
  }
  portEXIT_CRITICAL_ISR(&pulseMux);
}

uint32_t getPulseCountSnapshot() {
  uint32_t count;
  portENTER_CRITICAL(&pulseMux);
  count = pulseCount;
  portEXIT_CRITICAL(&pulseMux);
  return count;
}

uint32_t getAndResetPulseCount() {
  uint32_t count;
  portENTER_CRITICAL(&pulseMux);
  count = pulseCount;
  pulseCount = 0;
  portEXIT_CRITICAL(&pulseMux);
  return count;
}

// ======================================================
// WiFi & ThingSpeak 送信処理
// ======================================================
bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.println("WiFi disconnected. Reconnecting...");
  
  setLedColor(0, 0, 40); // 接続試行中は淡い青
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    if ((uint32_t)(millis() - startMs) > 5000) { 
      Serial.println("WiFi reconnection timeout (Skipped)");
      setLedColor(0, 0, 0);
      return false;
    }
  }
  Serial.println("WiFi Connected. IP: " + WiFi.localIP().toString());
  setLedColor(0, 0, 0);
  return true;
}

bool sendToThingSpeak(float cpm, float usvPerHour, uint32_t rawCount) {
  if (!ensureWiFiConnected()) return false;

  HTTPClient http;
  http.setTimeout(5000); 
  if (!http.begin(THINGSPEAK_URL)) return false;

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload;
  payload.reserve(128);
  payload += "api_key=" + String(THINGSPEAK_WRITE_API_KEY);
  payload += "&field1=" + String(cpm, 2);
  payload += "&field2=" + String(usvPerHour, 4);
  payload += "&field3=" + String(rawCount);

  int httpCode = http.POST(payload);
  String response = http.getString();
  http.end();

  if (httpCode == 200 && response.toInt() > 0) return true;
  return false;
}

// ======================================================
// setup
// ======================================================
void setup() {
  // AtomS3オブジェクトの初期化
  AtomS3.begin(true);            // AtomS3 Lite 初期化
  AtomS3.dis.setBrightness(80);  // 輝度設定
  
  setLedColor(0, 0, 0); // 初期状態は消灯

  Serial.begin(115200);
  delay(1000);

  Serial.println("\n======================================");
  Serial.println("GeigerCounter V1.1 + M5 AtomS3 Lite");
  Serial.println("AtomS3.dis Verified Control Mode");
  Serial.println("======================================");

  pinMode(GEIGER_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), onGeigerPulse, FALLING);

  ensureWiFiConnected();

  windowStartMs = millis();
  lastSerialMs = millis();
}

// ======================================================
// loop
// ======================================================
void loop() {
  // メインループ内でもボタン等の状態更新のためにAtomS3.update()を呼ぶ
  AtomS3.update();
  uint32_t nowMs = millis();

  // --- パルス検知時のLED点滅処理（loop内で安全に実行） ---
  if (pulseDetected) {
    portENTER_CRITICAL(&pulseMux);
    pulseDetected = false; // フラグをクリア
    portEXIT_CRITICAL(&pulseMux);

    // パルス検知時に一瞬黄色（赤+緑）に光らせる
    blinkLed(60, 60, 0, 1, 30); 
  }

  // 5秒ごとのシリアル出力
  if ((uint32_t)(nowMs - lastSerialMs) >= SERIAL_PERIOD_MS) {
    lastSerialMs = nowMs;
    uint32_t elapsedMs = (uint32_t)(nowMs - windowStartMs);
    uint32_t currentPulses = getPulseCountSnapshot();
    float cpmNow = (elapsedMs >= 1000) ? ((float)currentPulses * 60000.0f / (float)elapsedMs) : 0.0f;
    float usvNow = cpmNow / CPM_PER_USV_H;

    Serial.printf("[Live] Elapsed: %lu s, Counts: %lu, CPM: %.2f, uSv/h: %.4f\n", 
                  (unsigned long)(elapsedMs / 1000), (unsigned long)currentPulses, cpmNow, usvNow);
  }

  // 60秒ごとのThingSpeak送信
  if ((uint32_t)(nowMs - windowStartMs) >= LOG_PERIOD_MS) {
    uint32_t elapsedMs = (uint32_t)(nowMs - windowStartMs);
    windowStartMs = nowMs; 

    uint32_t count = getAndResetPulseCount();
    float cpm = (elapsedMs > 0) ? ((float)count * 60000.0f / (float)elapsedMs) : 0.0f;
    float usvPerHour = cpm / CPM_PER_USV_H;

    Serial.println("\n----- 60 sec result -----");
    bool uploaded = sendToThingSpeak(cpm, usvPerHour, count);
    Serial.printf("ThingSpeak upload: %s\n\n", uploaded ? "OK" : "NG");

    if (uploaded) {
      blinkLed(0, 80, 0, 3, 150); // 送信成功：緑
    } else {
      blinkLed(80, 0, 0, 3, 150); // 送信失敗：赤
    }
  }

  delay(10);
}
