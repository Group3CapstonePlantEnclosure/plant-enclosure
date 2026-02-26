/*
  Device: Arduino Uno R4 WiFi
  Role: UART Data Sender/Receiver (Replaces BLE)
  Hardware: VEML7700 (I2C), DHT11 (Pin 2), UART (TX/RX)
*/

#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <DHT.h>

// --- Configuration ---
#define DHTPIN 2
#define DHTTYPE DHT11

// Define the hardware serial port. 
// Change to Serial1 if your Uno R4 does not have a custom Serial2 mapped.
#define COMM_SERIAL Serial2 

// Initialize Sensors
DHT dht(DHTPIN, DHTTYPE);
Adafruit_VEML7700 veml = Adafruit_VEML7700();

unsigned long lastUpdate = 0;
const long updateInterval = 5000; // Send data every 5 seconds

void setup() {
  // Main serial for PC debugging
  Serial.begin(115200);
  while (!Serial);

  // Initialize UART communication with the ESP32
  // Ensure the ESP32 code is also set to 115200 baud for its Serial2
  COMM_SERIAL.begin(115200);

  // 1. Initialize Sensors
  dht.begin();
  if (!veml.begin()) {
    Serial.println("Error: VEML7700 not found. Check I2C wiring.");
    while (1); 
  }
  
  // Configure Lux Sensor for indoor lighting
  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("UART Hub active. Communicating with ESP32 via TX/RX...");
}

void loop() {
  unsigned long currentMillis = millis();

  // --- Task 1: Check for incoming messages from ESP32 ---
  // Using \n as the end-of-message marker
  if (COMM_SERIAL.available()) {
    String received = COMM_SERIAL.readStringUntil('\n');
    received.trim(); // Clean up hidden carriage returns
    if (received.length() > 0) {
      Serial.print("ESP32 Command: ");
      Serial.println(received);
    }
  }

  // --- Task 2: Check for manual Serial input to send to ESP32 ---
  if (Serial.available()) {
    String manualMsg = Serial.readStringUntil('\n');
    manualMsg.trim();
    if (manualMsg.length() > 0) {
      COMM_SERIAL.println(manualMsg);
      Serial.println("Manual Msg Sent to ESP32.");
    }
  }

  // --- Task 3: Send Sensor Data Packets ---
  if (currentMillis - lastUpdate >= updateInterval) {
    lastUpdate = currentMillis;
    sendSensorData();
  }
}

void sendSensorData() {
  float h = dht.readHumidity();
  float t = dht.readTemperature(); // Celsius
  float lux = veml.readLux();

  // Check if readings are valid before sending
  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }

  // Format: "T:22.5,H:45,L:350.2"
  String dataPacket = "T:" + String(t, 1) + ",H:" + String(h, 0) + ",L:" + String(lux, 1);
  
  Serial.print("Sending Packet over UART: ");
  Serial.println(dataPacket);

  // Push update to ESP32 over UART (println automatically adds \r\n)
  COMM_SERIAL.println(dataPacket);
}