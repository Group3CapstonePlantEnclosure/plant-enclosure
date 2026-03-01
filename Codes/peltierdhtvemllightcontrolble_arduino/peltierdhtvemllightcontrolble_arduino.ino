#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include "Adafruit_SHT4x.h"

#define COMM_SERIAL Serial1 

// Hardware Pins
#define PELTIER_IN1 4
#define PELTIER_IN2 5
#define LIGHT_PWM_PIN 9

Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// Calibration & Maintenance
float tempOffset = -7.5; //temp offset
unsigned long lastHeaterCycle = 0;
const unsigned long heaterInterval = 900000; // 1 hour
bool heaterActive = false;
unsigned long heaterStartTime = 0;

bool sht4_found = false, veml_found = false;
unsigned long lastUpdate = 0;
const long updateInterval = 10000; // 10 seconds for stability

void setup() {
  Serial.begin(115200);
  COMM_SERIAL.begin(115200);

  pinMode(PELTIER_IN1, OUTPUT);
  pinMode(PELTIER_IN2, OUTPUT);
  pinMode(LIGHT_PWM_PIN, OUTPUT);
  
  if (sht4.begin()) { sht4_found = true; sht4.setHeater(SHT4X_NO_HEATER); }
  if (veml.begin()) veml_found = true;

  Serial.println("System Online. Type 'CLEAN' to test SHT4x heater.");
}

void loop() {
  unsigned long currentMillis = millis();

  // --- Task 1: Check for Manual PC Serial Commands ---
  if (Serial.available()) {
    String pcCmd = Serial.readStringUntil('\n');
    pcCmd.trim();
    if (pcCmd.equalsIgnoreCase("CLEAN")) {
      startCleaningCycle();
    }
  }

  // --- Task 2: Automatic Hourly Cleaning ---
  if (currentMillis - lastHeaterCycle >= heaterInterval && !heaterActive) {
    startCleaningCycle();
  }

  // --- Task 3: Handle Heater Cooldown ---
if (heaterActive && (currentMillis - heaterStartTime > 10000)) { 
  Serial.println("Cooldown finished. Resuming accurate data.");
  heaterActive = false;
}

  // --- Task 4: Normal Operations ---
  if (!heaterActive) {
    // Listen for ESP32 Commands
    if (COMM_SERIAL.available()) {
      String received = COMM_SERIAL.readStringUntil('\n');
      received.trim();
      processESP32Command(received);
    }

    // Send Sensor Data to ESP32
    if (currentMillis - lastUpdate >= updateInterval) {
      lastUpdate = currentMillis;
      sendSensorData();
    }
  }
}

void startCleaningCycle() {
  if (!sht4_found) return;
  
  Serial.println("!!! SHT4x CLEANING START (1s Blast) !!!");
  
  // 1. Tell the library to use the heater for the NEXT measurement
  sht4.setHeater(SHT4X_HIGH_HEATER_1S);
  
  // 2. Trigger the measurement. This is where the 1s pulse actually happens.
  // We don't care about these specific values because they will be very hot.
  sensors_event_t h, t;
  sht4.getEvent(&h, &t); 
  
  // 3. IMMEDIATELY set it back to NO_HEATER mode
  // This prevents future measurements from being hot.
  sht4.setHeater(SHT4X_NO_HEATER);
  
  // 4. Start the cooldown timer (we wait 10 seconds for the tiny PCB to cool)
  heaterStartTime = millis();
  heaterActive = true;
  lastHeaterCycle = millis();
  
  // Tell ESP32 we are in maintenance mode
  COMM_SERIAL.println("STATUS:CLEANING");
  Serial.println("Blast complete. Cooling down for 10 seconds...");
}

void processESP32Command(String cmd) {
  if (cmd == "CMD:COOL") { digitalWrite(PELTIER_IN1, HIGH); digitalWrite(PELTIER_IN2, LOW); }
  else if (cmd == "CMD:HEAT") { digitalWrite(PELTIER_IN1, LOW); digitalWrite(PELTIER_IN2, HIGH); }
  else if (cmd == "CMD:PELTIER_OFF") { digitalWrite(PELTIER_IN1, LOW); digitalWrite(PELTIER_IN2, LOW); }
  else if (cmd.startsWith("CMD:LIGHT,")) {
    int pwmValue = cmd.substring(10).toInt(); 
    analogWrite(LIGHT_PWM_PIN, pwmValue);
  }
}

void sendSensorData() {
  float t = 0.0, h = 0.0, lux = 0.0;
  if (sht4_found) {
    sensors_event_t humidity, temp;
    sht4.getEvent(&humidity, &temp);
    t = ((temp.temperature * 1.8) + 32.0) + tempOffset;
    h = humidity.relative_humidity;
  }
  if (veml_found) lux = veml.readLux();

  String dataPacket = "T:" + String(t, 1) + ",H:" + String(h, 0) + ",L:" + String(lux, 1);
  COMM_SERIAL.println(dataPacket);
  Serial.println("UART Packet: " + dataPacket);
}