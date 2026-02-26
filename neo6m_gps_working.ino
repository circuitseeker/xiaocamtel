#include <TinyGPSPlus.h>

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

#define RXD2 44
#define TXD2 43

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      Serial.print("Latitude: ");
      Serial.println(gps.location.lat(), 6);

      Serial.print("Longitude: ");
      Serial.println(gps.location.lng(), 6);
      Serial.println("---------------------");
    }
  }
}
