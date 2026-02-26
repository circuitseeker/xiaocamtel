// Pins
#define TOUCH_PIN D2      // touch sensor connected here
#define MOTOR_PIN D3      // vibration motor here

void setup() {
  pinMode(TOUCH_PIN, INPUT);     // touch sensor as input
  pinMode(MOTOR_PIN, OUTPUT);    // motor as output

  digitalWrite(MOTOR_PIN, LOW);  // motor off initially
  Serial.begin(115200);
}

void loop() {

  int touchState = digitalRead(TOUCH_PIN);

  if (touchState == HIGH) {   // touch detected
    digitalWrite(MOTOR_PIN, HIGH);  // motor ON
    Serial.println("Touch detected → Motor ON");
  } 
  else {
    digitalWrite(MOTOR_PIN, LOW);   // motor OFF
  }

  delay(50);
}



////#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <UniversalTelegramBot.h>

// WiFi credentials
const char* ssid = "Helium iot";
const char* password = "upsurge.io";

// Telegram
String chatId = "2049241502";
String BOTtoken = "7966378089:AAEBIveW8ku4oddEebJ949jWw0TwMoji4M0";

WiFiClientSecure clientTCP;

// XIAO ESP32S3 camera pins
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


// 📸 SEND PHOTO WITH "HELLO"
String sendPhotoTelegram() {
  const char* myDomain = "api.telegram.org";
  String getAll = "";
  String getBody = "";

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return "Camera failed";
  }

  Serial.printf("Photo size: %u bytes\n", fb->len);

  if (clientTCP.connect(myDomain, 443)) {
    Serial.println("Connected to Telegram");

    String caption = "hello";

    String head = "--IotCircuitHub\r\nContent-Disposition: form-data; name=\"chat_id\";\r\n\r\n" + chatId +
                  "\r\n--IotCircuitHub\r\nContent-Disposition: form-data; name=\"caption\";\r\n\r\n" + caption +
                  "\r\n--IotCircuitHub\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"esp32.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

    String tail = "\r\n--IotCircuitHub--\r\n";

    uint32_t imageLen = fb->len;
    uint32_t totalLen = imageLen + head.length() + tail.length();

    clientTCP.println("POST /bot" + BOTtoken + "/sendPhoto HTTP/1.1");
    clientTCP.println("Host: " + String(myDomain));
    clientTCP.println("Content-Length: " + String(totalLen));
    clientTCP.println("Content-Type: multipart/form-data; boundary=IotCircuitHub");
    clientTCP.println();
    clientTCP.print(head);

    uint8_t *fbBuf = fb->buf;
    size_t fbLen = fb->len;

    for (size_t n = 0; n < fbLen; n += 1024) {
      if (n + 1024 < fbLen) {
        clientTCP.write(fbBuf, 1024);
        fbBuf += 1024;
      } else {
        size_t remainder = fbLen % 1024;
        clientTCP.write(fbBuf, remainder);
      }
    }

    clientTCP.print(tail);
    esp_camera_fb_return(fb);

    // wait response
    long startTimer = millis();
    while (millis() - startTimer < 10000) {
      while (clientTCP.available()) {
        char c = clientTCP.read();
        getBody += String(c);
      }
      if (getBody.length() > 0) break;
    }

    clientTCP.stop();
    Serial.println("Telegram response:");
    Serial.println(getBody);
  } 
  else {
    Serial.println("Telegram connection failed");
    esp_camera_fb_return(fb);
  }

  return getBody;
}



void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid, password);
  clientTCP.setInsecure();   // SSL fix

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());

  // Camera config
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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    while (true);
  }

  Serial.println("Camera ready");
}



void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi reconnecting...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("Reconnected");
  }

  Serial.println("\nSending photo to Telegram...");
  sendPhotoTelegram();

  delay(30000); // every 30 sec
}////