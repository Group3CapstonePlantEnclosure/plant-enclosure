/*
  Device: Arduino Uno R4 WiFi
  Role: BLE Peripheral (Server)
  Hardware: VEML7700 (I2C), DHT11 (Pin 2)
  Updated: 2-Way Chat Enabled
*/

#include <ArduinoBLE.h>
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <DHT.h>

// --- Config ---
#define DHTPIN 2
#define DHTTYPE DHT11

// UUIDs (Universally Unique Identifiers)
const char* serviceUUID = "19B10000-E8F2-537E-4F6C-D104768A1214";
const char* sensorCharUUID = "19B10001-E8F2-537E-4F6C-D104768A1214"; // R4 -> ESP32
const char* messageCharUUID = "19B10002-E8F2-537E-4F6C-D104768A1214"; // ESP32 -> R4

// Initialize Sensors
DHT dht(DHTPIN, DHTTYPE);
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// BLE Service & Characteristics
BLEService customService(serviceUUID);
// "Notify" allows the ESP32 to get updates automatically
BLEStringCharacteristic sensorCharacteristic(sensorCharUUID, BLERead | BLENotify, 50);
// "Write" allows the ESP32 to send data here
BLEStringCharacteristic messageCharacteristic(messageCharUUID, BLEWrite, 50);

long lastUpdate = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // Start Sensors
  dht.begin();
  if (!veml.begin()) {
    Serial.println("VEML7700 not found!");
    while (1);
  }
  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_800MS);

  // Start BLE
  if (!BLE.begin()) {
    Serial.println("starting BLE failed!");
    while (1);
  }

  BLE.setLocalName("UnoR4_Sensor");
  BLE.setAdvertisedService(customService);

  // Add characteristics to the service
  customService.addCharacteristic(sensorCharacteristic);
  customService.addCharacteristic(messageCharacteristic);

  // Add service and start advertising
  BLE.addService(customService);
  BLE.advertise();

  Serial.println("BLE Peripheral active. Type a message to send to ESP32.");
}

void loop() {
  BLEDevice central = BLE.central();

  if (central) {
    Serial.print("Connected to central: ");
    Serial.println(central.address());

    while (central.connected()) {
      
      // 1. Check if ESP32 sent a message
      if (messageCharacteristic.written()) {
        String receivedMessage = messageCharacteristic.value();
        Serial.print("Message from ESP32: ");
        Serial.println(receivedMessage);
      }

      // 2. Check if YOU typed a message in Serial Monitor
      if (Serial.available()) {
        String manualMsg = Serial.readStringUntil('\n');
        manualMsg.trim(); // Remove whitespace/newlines

        if (manualMsg.length() > 0) {
          Serial.print("Sending Manual Msg: ");
          Serial.println(manualMsg);
          sensorCharacteristic.writeValue(manualMsg);
        }
      }

      // 3. Send Sensor Data every 5 seconds
      long currentMillis = millis();
      if (currentMillis - lastUpdate >= 5000) {
        lastUpdate = currentMillis;
        sendSensorData();
      }
    }
    Serial.println("Disconnected");
  }
}

void sendSensorData() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  float lux = veml.readLux();

  // Format data as a single string: "T:24.5,H:60,L:120"
  String dataPacket = "T:" + String(temp, 1) + ",H:" + String(hum, 0) + ",L:" + String(lux, 1);
  
  Serial.print("Sending Sensor Data: ");
  Serial.println(dataPacket);

  // Update the characteristic (ESP32 will receive this via Notify)
  sensorCharacteristic.writeValue(dataPacket);
}