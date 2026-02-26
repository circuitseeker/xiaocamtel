#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <TinyGPSPlus.h>

// --- WiFi / Telegram ---
const char* ssid = "Helium iot";
const char* password = "upsurge.io";
const char* chatId = "2049241502";
const char* BOTtoken = "7966378089:AAEBIveW8ku4oddEebJ949jWw0TwMoji4M0";
const char* telegramHost = "api.telegram.org";

WiFiClientSecure clientTCP;

// --- Pins ---
#define TOUCH_PIN   D2
#define MOTOR_PIN   D3
#define STOP_BUTTON D8

// --- GPS ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
#define RXD2 44
#define TXD2 43

// --- Heart sensor ---
MAX30105 particleSensor;
static const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
unsigned long lastBeat = 0;
float beatsPerMinute;
int beatAvg;

// --- Timing (non-blocking replacements for delay()) ---
unsigned long lastAlertTime = 0;
unsigned long motorOnTime = 0;
unsigned long lastHRPrint = 0;
static const unsigned long ALERT_INTERVAL_MS  = 7000;
static const unsigned long MOTOR_BUZZ_MS      = 1000;
static const unsigned long HR_PRINT_INTERVAL  = 1000;
static const int           HR_THRESHOLD       = 120; // raised from 90 to reduce false alarms

// --- State ---
bool emergencyActive = false;
bool motorBuzzing = false;

// --- Camera pins (XIAO ESP32S3) ---
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40
#define SIOC_GPIO_NUM    39
#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15
#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

// ==================== Helpers ====================

// Ensure WiFi stays connected
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("WiFi lost, reconnecting...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    // keep GPS fed while waiting
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("WiFi reconnected");
  else
    Serial.println("WiFi reconnect failed");
}

// URL-encode a string for safe GET params
String urlEncode(const String& str) {
  String encoded;
  encoded.reserve(str.length() * 2);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

void sendTelegramMessage(const char* text) {
  ensureWiFi();
  if (!clientTCP.connect(telegramHost, 443)) {
    Serial.println("Telegram msg connect failed");
    return;
  }
  String url = "/bot";
  url += BOTtoken;
  url += "/sendMessage?chat_id=";
  url += chatId;
  url += "&text=";
  url += urlEncode(String(text));

  clientTCP.print("GET ");
  clientTCP.print(url);
  clientTCP.println(" HTTP/1.1");
  clientTCP.println("Host: api.telegram.org");
  clientTCP.println("Connection: close");
  clientTCP.println();

  // wait briefly for response to confirm delivery
  unsigned long t = millis();
  while (clientTCP.connected() && millis() - t < 2000) {
    if (clientTCP.available()) {
      String line = clientTCP.readStringUntil('\n');
      if (line.startsWith("HTTP/1.1")) {
        Serial.print("Telegram msg response: ");
        Serial.println(line);
        break;
      }
    }
  }
  clientTCP.stop();
}

// Capture a fresh photo (discards stale buffered frame first)
camera_fb_t* captureFreshPhoto() {
  camera_fb_t* stale = esp_camera_fb_get();
  if (stale) esp_camera_fb_return(stale);
  return esp_camera_fb_get();
}

void sendPhotoTelegram(const char* msg) {
  ensureWiFi();

  // Build location string
  char locBuf[100];
  if (gps.location.isValid()) {
    snprintf(locBuf, sizeof(locBuf),
             "\nhttps://maps.google.com/?q=%.6f,%.6f",
             gps.location.lat(), gps.location.lng());
  } else {
    snprintf(locBuf, sizeof(locBuf), "\nGPS not fixed");
  }

  // Build caption
  String caption = String(msg) + locBuf;

  camera_fb_t* fb = captureFreshPhoto();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  if (!clientTCP.connect(telegramHost, 443)) {
    esp_camera_fb_return(fb);
    Serial.println("Telegram photo connect failed");
    return;
  }

  // Multipart boundaries
  String head = "--AaB03x\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  head += chatId;
  head += "\r\n--AaB03x\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n";
  head += caption;
  head += "\r\n--AaB03x\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

  static const char tail[] = "\r\n--AaB03x--\r\n";
  uint32_t totalLen = head.length() + fb->len + strlen(tail);

  // Send HTTP headers
  clientTCP.print("POST /bot");
  clientTCP.print(BOTtoken);
  clientTCP.println("/sendPhoto HTTP/1.1");
  clientTCP.println("Host: api.telegram.org");
  clientTCP.print("Content-Length: ");
  clientTCP.println(totalLen);
  clientTCP.println("Content-Type: multipart/form-data; boundary=AaB03x");
  clientTCP.println();

  // Send multipart head
  clientTCP.print(head);

  // Send JPEG in chunks (fixed: handles exact multiples of 1024)
  const size_t CHUNK = 1024;
  uint8_t* buf = fb->buf;
  size_t remaining = fb->len;
  while (remaining > 0) {
    size_t toSend = (remaining >= CHUNK) ? CHUNK : remaining;
    clientTCP.write(buf, toSend);
    buf += toSend;
    remaining -= toSend;
  }

  // Send multipart tail
  clientTCP.print(tail);

  // Check response
  unsigned long t = millis();
  while (clientTCP.connected() && millis() - t < 3000) {
    if (clientTCP.available()) {
      String line = clientTCP.readStringUntil('\n');
      if (line.startsWith("HTTP/1.1")) {
        Serial.print("Telegram photo response: ");
        Serial.println(line);
        break;
      }
    }
  }

  esp_camera_fb_return(fb);
  clientTCP.stop();
  Serial.print("Sent photo: ");
  Serial.println(caption);
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);

  pinMode(TOUCH_PIN, INPUT);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(STOP_BUTTON, INPUT_PULLUP);

  // WiFi
  WiFi.begin(ssid, password);
  clientTCP.setInsecure();
  Serial.print("Connecting WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  else
    Serial.println("\nWiFi failed (will retry in loop)");

  // GPS
  gpsSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // Camera
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2; // double buffer for fresher frames
  } else {
    config.frame_size   = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    while (true) delay(1000);
  }

  // MAX30105
  Wire.begin();
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 not found. Check wiring.");
    while (true) delay(1000);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("SYSTEM READY");
}

// ==================== Main Loop (non-blocking) ====================
void loop() {
  unsigned long now = millis();

  // --- Always feed GPS ---
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // --- Stop button (active LOW with pullup) ---
  if (digitalRead(STOP_BUTTON) == LOW) {
    if (emergencyActive) {
      emergencyActive = false;
      motorBuzzing = false;
      digitalWrite(MOTOR_PIN, LOW);
      sendTelegramMessage("I am OK");
      Serial.println("Emergency stopped by STOP button");
    }
    delay(300); // debounce
    return;     // skip rest of this loop iteration
  }

  // --- Touch trigger ---
  if (digitalRead(TOUCH_PIN) == HIGH && !emergencyActive) {
    emergencyActive = true;
    motorBuzzing = true;
    motorOnTime = now;
    digitalWrite(MOTOR_PIN, HIGH);
    lastAlertTime = 0; // force immediate first alert
    Serial.println("Touch -> emergency mode");
    delay(200); // debounce
  }

  // --- Heart rate sensor (unchanged algorithm) ---
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue)) {
    long delta = now - lastBeat;
    lastBeat = now;
    beatsPerMinute = 60.0f / (delta / 1000.0f);

    if (beatsPerMinute > 20 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;

      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++)
        beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  // --- HR serial output (throttled to 1/sec instead of every loop) ---
  if (now - lastHRPrint >= HR_PRINT_INTERVAL) {
    lastHRPrint = now;
    Serial.print("BPM=");
    Serial.print(beatsPerMinute);
    Serial.print(" Avg=");
    Serial.print(beatAvg);
    if (irValue < 50000) Serial.print(" NF");
    Serial.println();
  }

  // --- HR threshold -> emergency ---
  if (beatAvg > HR_THRESHOLD && irValue > 50000 && !emergencyActive) {
    emergencyActive = true;
    motorBuzzing = true;
    motorOnTime = now;
    digitalWrite(MOTOR_PIN, HIGH);
    lastAlertTime = 0;
    Serial.println("High BPM -> emergency mode");
  }

  // --- Motor buzz timeout (non-blocking) ---
  if (motorBuzzing && now - motorOnTime >= MOTOR_BUZZ_MS) {
    digitalWrite(MOTOR_PIN, LOW);
    motorBuzzing = false;
  }

  // --- Emergency alert sending (non-blocking interval) ---
  if (emergencyActive && now - lastAlertTime >= ALERT_INTERVAL_MS) {
    lastAlertTime = now;

    char label[40];
    if (beatAvg > HR_THRESHOLD && irValue > 50000)
      snprintf(label, sizeof(label), "HR HIGH: %d", beatAvg);
    else
      snprintf(label, sizeof(label), "EMERGENCY");

    sendPhotoTelegram(label);
  }
}
