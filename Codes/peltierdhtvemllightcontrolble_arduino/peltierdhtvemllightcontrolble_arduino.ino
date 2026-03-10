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

// ------------------- AUTO LIGHT CONTROL -------------------
float lowLuxThreshold  = 150.0;
float highLuxThreshold = 250.0;

int level = 3;
bool ledOn = false;

int pwmForLevel(int lvl) {
  if (lvl == 1) return 25;2
  if (lvl == 2) return 100;
  return 255;
}

void applyPwm(int pwm) {
  analogWrite(LIGHT_PWM_PIN, constrain(pwm, 0, 255));
}

void turnOn() {
  int pwm = pwmForLevel(level);
  applyPwm(pwm);
  ledOn = true;

  Serial.print("LED ON | Lux below ");
  Serial.println(lowLuxThreshold);
}

void turnOff() {
  applyPwm(0);
  ledOn = false;

  Serial.print("LED OFF | Lux above ");
  Serial.println(highLuxThreshold);
}

void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    Serial.println("Commands:");
    Serial.println("  low <lux>");
    Serial.println("  high <lux>");
    Serial.println("  lvl <1|2|3>");
    Serial.println("  show");
    Serial.println("  CLEAN");
    return;
  }

  if (cmd == "show") {
    Serial.print("Low threshold: ");
    Serial.println(lowLuxThreshold);
    Serial.print("High threshold: ");
    Serial.println(highLuxThreshold);
    Serial.print("Level: ");
    Serial.println(level);
    Serial.print("PWM: ");
    Serial.println(pwmForLevel(level));
    return;
  }

  if (cmd == "CLEAN") {
    startCleaningCycle();
    return;
  }

  int sp = cmd.indexOf(' ');
  String key = (sp == -1) ? cmd : cmd.substring(0, sp);
  String val = (sp == -1) ? ""  : cmd.substring(sp + 1);

  key.trim();
  val.trim();

  if (key == "low") {
    float v = val.toFloat();
    if (v >= 0) {
      lowLuxThreshold = v;
      Serial.print("Low threshold set to ");
      Serial.println(lowLuxThreshold);
    }
  }

  else if (key == "high") {
    float v = val.toFloat();
    if (v >= 0) {
      highLuxThreshold = v;
      Serial.print("High threshold set to ");
      Serial.println(highLuxThreshold);
    }
  }

  else if (key == "lvl") {
    int lv = val.toInt();
    if (lv >= 1 && lv <= 3) {
      level = lv;
      Serial.print("Level set to ");
      Serial.println(level);

      if (ledOn) {
        applyPwm(pwmForLevel(level));
      }
    }
  }

  else {
    Serial.println("Unknown command. Type: help");
  }
}
// -----------------------------------------------------------


// Calibration & Maintenance
float tempOffset = -7.5;
unsigned long lastHeaterCycle = 0;
const unsigned long heaterInterval = 900000;

bool heaterActive = false;
unsigned long heaterStartTime = 0;

bool sht4_found = false, veml_found = false;

unsigned long lastUpdate = 0;
const long updateInterval = 10000;

void setup() {

  Serial.begin(115200);
  COMM_SERIAL.begin(115200);

  pinMode(PELTIER_IN1, OUTPUT);
  pinMode(PELTIER_IN2, OUTPUT);
  pinMode(LIGHT_PWM_PIN, OUTPUT);

  applyPwm(0);

  if (sht4.begin()) {
    sht4_found = true;
    sht4.setHeater(SHT4X_NO_HEATER);
  }

  if (veml.begin()) {
    veml_found = true;
  }

  Serial.println("System Online.");
}

void loop() {

  unsigned long currentMillis = millis();

  // Handle PC serial commands
  handleSerial();

  // --- Automatic Hourly Cleaning ---
  if (currentMillis - lastHeaterCycle >= heaterInterval && !heaterActive) {
    startCleaningCycle();
  }

  // --- Heater Cooldown ---
  if (heaterActive && (currentMillis - heaterStartTime > 10000)) {
    Serial.println("Cooldown finished. Resuming accurate data.");
    heaterActive = false;
  }

  if (!heaterActive) {

    // Listen for ESP32 Commands
    if (COMM_SERIAL.available()) {
      String received = COMM_SERIAL.readStringUntil('\n');
      received.trim();
      processESP32Command(received);
    }

    // Sensor update
    if (currentMillis - lastUpdate >= updateInterval) {

      lastUpdate = currentMillis;

      float lux = 0;
      if (veml_found) lux = veml.readLux();

      // -------- AUTO LIGHT CONTROL --------
      if (!ledOn && lux <= lowLuxThreshold) {
        turnOn();
      }

      if (ledOn && lux >= highLuxThreshold) {
        turnOff();
      }
      // ------------------------------------

      sendSensorData(lux);
    }
  }
}

void startCleaningCycle() {

  if (!sht4_found) return;

  Serial.println("!!! SHT4x CLEANING START (1s Blast) !!!");

  sht4.setHeater(SHT4X_HIGH_HEATER_1S);

  sensors_event_t h, t;
  sht4.getEvent(&h, &t);

  sht4.setHeater(SHT4X_NO_HEATER);

  heaterStartTime = millis();
  heaterActive = true;
  lastHeaterCycle = millis();

  COMM_SERIAL.println("STATUS:CLEANING");

  Serial.println("Blast complete. Cooling down for 10 seconds...");
}

void processESP32Command(String cmd) {

  if (cmd == "CMD:COOL") {
    digitalWrite(PELTIER_IN1, HIGH);
    digitalWrite(PELTIER_IN2, LOW);
  }

  else if (cmd == "CMD:HEAT") {
    digitalWrite(PELTIER_IN1, LOW);
    digitalWrite(PELTIER_IN2, HIGH);
  }

  else if (cmd == "CMD:PELTIER_OFF") {
    digitalWrite(PELTIER_IN1, LOW);
    digitalWrite(PELTIER_IN2, LOW);
  }

  else if (cmd.startsWith("CMD:LIGHT,")) {

    int pwmValue = cmd.substring(10).toInt();
    analogWrite(LIGHT_PWM_PIN, pwmValue);
  }
}

void sendSensorData(float lux) {

  float t = 0.0;
  float h = 0.0;

  if (sht4_found) {

    sensors_event_t humidity, temp;

    sht4.getEvent(&humidity, &temp);

    t = ((temp.temperature * 1.8) + 32.0) + tempOffset;
    h = humidity.relative_humidity;
  }

  String dataPacket =
    "T:" + String(t,1) +
    ",H:" + String(h,0) +
    ",L:" + String(lux,1);

  COMM_SERIAL.println(dataPacket);

  Serial.println("UART Packet: " + dataPacket);
}