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

String chatId = "2049241502";
String BOTtoken = "7966378089:AAEBIveW8ku4oddEebJ949jWw0TwMoji4M0";

WiFiClientSecure clientTCP;

// --- Pins ---
#define TOUCH_PIN D2
#define MOTOR_PIN D3
#define STOP_BUTTON D8

// --- GPS ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
#define RXD2 44
#define TXD2 43

// --- Heart sensor (YOUR ORIGINAL) ---
MAX30105 particleSensor;
const byte RATE_SIZE = 4; //Increase this for more averaging. 4 is good.
byte rates[RATE_SIZE]; //Array of heart rates
byte rateSpot = 0;
long lastBeat = 0; //Time at which the last beat occurred

float beatsPerMinute;
int beatAvg;

// --- Flags ---
bool emergencyActive = false;
bool justActivated = false; // to vibrate once on activation

// --- Camera pins (XIAO ESP32S3) ---
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      10
#define SIOD_GPIO_NUM      40
#define SIOC_GPIO_NUM      39
#define Y9_GPIO_NUM        48
#define Y8_GPIO_NUM        11
#define Y7_GPIO_NUM        12
#define Y6_GPIO_NUM        14
#define Y5_GPIO_NUM        16
#define Y4_GPIO_NUM        18
#define Y3_GPIO_NUM        17
#define Y2_GPIO_NUM        15
#define VSYNC_GPIO_NUM     38
#define HREF_GPIO_NUM      47
#define PCLK_GPIO_NUM      13

// ---------- Telegram helpers ----------
void sendTelegramMessage(String text){
  const char* host = "api.telegram.org";
  if(clientTCP.connect(host,443)){
    String url = "/bot"+BOTtoken+"/sendMessage?chat_id="+chatId+"&text="+text;
    clientTCP.println("GET " + url + " HTTP/1.1");
    clientTCP.println("Host: api.telegram.org");
    clientTCP.println("Connection: close");
    clientTCP.println();
  }
  delay(300);
  clientTCP.stop();
}

void sendPhotoTelegram(String msg){
  String locationMsg="";
  if(gps.location.isValid()){
    locationMsg="\n📍https://maps.google.com/?q="+
      String(gps.location.lat(),6)+","+String(gps.location.lng(),6);
  } else {
    locationMsg="\n📍GPS not fixed";
  }

  String finalMsg = msg + locationMsg;
  const char* host = "api.telegram.org";

  camera_fb_t * fb = esp_camera_fb_get();
  if(!fb){
    Serial.println("Camera capture failed");
    return;
  }

  if(clientTCP.connect(host,443)){
    String head = "--AaB03x\r\nContent-Disposition: form-data; name=\"chat_id\";\r\n\r\n"+chatId+
      "\r\n--AaB03x\r\nContent-Disposition: form-data; name=\"caption\";\r\n\r\n"+finalMsg+
      "\r\n--AaB03x\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

    String tail = "\r\n--AaB03x--\r\n";
    uint32_t totalLen = fb->len + head.length() + tail.length();

    clientTCP.println("POST /bot"+BOTtoken+"/sendPhoto HTTP/1.1");
    clientTCP.println("Host: api.telegram.org");
    clientTCP.println("Content-Length: " + String(totalLen));
    clientTCP.println("Content-Type: multipart/form-data; boundary=AaB03x");
    clientTCP.println();
    clientTCP.print(head);

    uint8_t *fbBuf = fb->buf;
    size_t fbLen = fb->len;

    for(size_t n=0; n<fbLen; n+=1024){
      if(n + 1024 < fbLen){
        clientTCP.write(fbBuf, 1024);
        fbBuf += 1024;
      } else {
        clientTCP.write(fbBuf, fbLen % 1024);
      }
    }

    clientTCP.print(tail);
    esp_camera_fb_return(fb);
    clientTCP.stop();
    Serial.println("Sent photo with message:");
    Serial.println(finalMsg);
  } else {
    esp_camera_fb_return(fb);
    Serial.println("Telegram connect failed");
  }
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);

  pinMode(TOUCH_PIN, INPUT);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(STOP_BUTTON, INPUT_PULLUP);

  // WiFi
  WiFi.begin(ssid, password);
  clientTCP.setInsecure();
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  // GPS
  gpsSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // Camera init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if(psramFound()){
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK){
    Serial.println("Camera init failed");
    while(true) { delay(1000); }
  }

  // MAX30105 init (your original)
  Wire.begin();
  if(!particleSensor.begin(Wire, I2C_SPEED_FAST)){
    Serial.println("MAX30105 was not found. Please check wiring/power.");
    while(1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("SYSTEM READY");
}

// ---------- Main loop ----------
void loop(){
  // Keep GPS feeding
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // Check STOP button immediately (if pressed, stop emergency and notify)
  if (digitalRead(STOP_BUTTON) == LOW){
    if (emergencyActive){
      emergencyActive = false;
      sendTelegramMessage("✅ I am OK");
      Serial.println("Emergency stopped by STOP button");
      delay(1000);
    }
    // simple debounce delay to avoid accidental double press
    delay(300);
  }

  // TOUCH trigger sets emergency (vibrate 1s)
  if (digitalRead(TOUCH_PIN) == HIGH){
    if(!emergencyActive){
      emergencyActive = true;
      justActivated = true; // flag to vibrate once
      Serial.println("Touch triggered -> activating emergency mode");
    }
    // small debounce
    delay(200);
  }

  // HEART SENSOR (YOUR EXACT ORIGINAL CODE - untouched)
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue) == true)
  {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20)
    {
      rates[rateSpot++] = (byte)beatsPerMinute; //Store this reading in the array
      rateSpot %= RATE_SIZE; //Wrap variable

      //Take average of readings
      beatAvg = 0;
      for (byte x = 0 ; x < RATE_SIZE ; x++)
        beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  // Live serial output (same as your format)

  Serial.print(", BPM=");
  Serial.print(beatsPerMinute);

  if (irValue < 50000) Serial.print("NF");
  Serial.println();

  // Heart threshold check -> set emergency (only set flag, do not change detection logic)
  if (beatAvg > 90 && irValue > 50000){
    if(!emergencyActive){
      emergencyActive = true;
      justActivated = true;
      Serial.println("High BPM triggered -> activating emergency mode");
    }
  }

  // If emergencyActive -> send alerts continuously until STOP pressed
  if (emergencyActive){
    // vibrate once when emergency first becomes active
    if (justActivated){
      digitalWrite(MOTOR_PIN, HIGH);
      delay(1000);
      digitalWrite(MOTOR_PIN, LOW);
      justActivated = false;
    }

    // send alert (photo + location + label)
    // label distinguishes touch vs HR using beatAvg when available
    String label = "🚨 EMERGENCY";
    // if heart triggered recently and beatAvg > 90, send HR label
    if (beatAvg > 90 && irValue > 50000) label = "❤️ HR HIGH: " + String(beatAvg);

    sendPhotoTelegram(label);

    // while in emergency mode, check STOP button frequently and keep GPS feeding
    // wait for ~7 seconds total but check every 200ms for STOP
    const int checks = 35; // 35 * 200ms = 7000ms
    for (int i = 0; i < checks; ++i){
      // update GPS stream so location may improve between sends
      while (gpsSerial.available()) gps.encode(gpsSerial.read());

      if (digitalRead(STOP_BUTTON) == LOW){
        emergencyActive = false;
        sendTelegramMessage("✅ I am OK");
        Serial.println("Emergency stopped by STOP button during active mode");
        delay(300);
        break;
      }
      delay(200);
    }
  }

  // small loop delay
  delay(20);
}
