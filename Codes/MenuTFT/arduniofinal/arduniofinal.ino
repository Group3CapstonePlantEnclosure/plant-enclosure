#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include "Adafruit_SHT4x.h"
#include <BTS7960.h>
#include <elapsedMillis.h>

// ------------------- UART LINK -------------------
// UNO R4 WiFi D0/D1 hardware UART is Serial1.
#define COMM_SERIAL Serial1

// ------------------- HARDWARE PINS -------------------
#define LIGHT_PWM_PIN 7
const uint8_t R_EN = 8;
const uint8_t L_EN = 9;
const uint8_t RPWM = 10;
const uint8_t LPWM = 11;
// Physical fan wiring is swapped relative to original labels, so map pins accordingly.
const uint8_t TOP_FAN_PIN = 6;
const uint8_t BOTTOM_FAN_PIN = 5;
const uint8_t MIST_PIN = 4;
const uint8_t WATER_PUMP_PIN = 12;
// KB2040 I2C Peripheral Address
#define KB2040_I2C_ADDR 0x08


// Soil moisture is not wired yet. Keep pin placeholder for later.
const int SOIL_PIN = A0;

// ------------------- ACTUATOR OBJECTS -------------------
BTS7960 motorController(L_EN, R_EN, LPWM, RPWM);
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// ------------------- CONTROL STATE -------------------
enum PeltierMode { PELTIER_OFF, PELTIER_HEAT, PELTIER_COOL };
PeltierMode currentMode = PELTIER_OFF;

elapsedMillis peltierPwmTimer;
const unsigned long PWM_PERIOD_MS = 2000;
const unsigned long PWM_ON_MS = (unsigned long)(PWM_PERIOD_MS * 0.83f);

unsigned long peltierPulseUntilMs = 0;
unsigned long waterPumpUntilMs = 0;

bool mistOn = false;
bool fanOn = false;
bool lightOn = false;
int lightPwm = 0;
int lightLevel = 3;
bool bottomFanOn = false;

const unsigned long PELTIER_PULSE_MS = 20000UL; // <-- CHANGED to 20 seconds
const unsigned long PELTIER_SWITCH_GUARD_MS = 5000UL; // Still enforces a 5s safety cooldown between hot/cold
bool pendingPeltierChange = false;
PeltierMode pendingPeltierMode = PELTIER_OFF;
unsigned long pendingPeltierPulseMs = 0;
unsigned long pendingPeltierApplyMs = 0;

// ------------------- SENSOR TELEMETRY -------------------
float currentTempF = 0.0f;
float currentHumidity = 0.0f;
float currentLux = 0.0f;
float currentSoil = -1.0f; // -1 means "not wired"
bool sht4Found = false;
bool vemlFound = false;
unsigned long lastTelemetryMs = 0;
const unsigned long TELEMETRY_INTERVAL_MS = 10000;
const float TEMP_OFFSET_F = -7.5f;

// ------------------- HELPERS -------------------
static void applyLightPwm(int pwm) {
  lightPwm = constrain(pwm, 0, 255);
  analogWrite(LIGHT_PWM_PIN, lightPwm);
  lightOn = (lightPwm > 0);
}

static void setMist(bool enable) {
  mistOn = enable;
  // Mist is physically wired to the Arduino, so we use digitalWrite
  digitalWrite(MIST_PIN, mistOn ? HIGH : LOW);
}

static void setFan(bool enable) {
  fanOn = enable;
  analogWrite(TOP_FAN_PIN, fanOn ? 255 : 0);
}

static void setBottomFan(bool enable) {
  bottomFanOn = enable;
  analogWrite(BOTTOM_FAN_PIN, bottomFanOn ? 255 : 0);
}

static void setBottomFanPwm(int pwm) {
  int out = constrain(pwm, 0, 255);
  bottomFanOn = (out > 0);
  analogWrite(BOTTOM_FAN_PIN, out);
}

static int pwmForLevel(int lvl) {
  if(lvl <= 1) return 80;
  if(lvl == 2) return 160;
  return 255;
}

static void setPeltierMode(PeltierMode mode) {
  if(currentMode == mode) return;

  currentMode = mode;
  if(currentMode == PELTIER_OFF) {
    motorController.Stop();
    setFan(false);
    setBottomFanPwm(0);
    return;
  }

  // Run both fans while peltier is active.
  setFan(true);
  if(currentMode == PELTIER_HEAT) setBottomFanPwm(153); // ~60%
  else setBottomFanPwm(255);
  peltierPwmTimer = 0;
}

static void applyPeltierCommand(PeltierMode mode, unsigned long pulseMs) {
  pendingPeltierChange = false;

  if(mode == PELTIER_OFF) {
    peltierPulseUntilMs = 0;
    setPeltierMode(PELTIER_OFF);
    return;
  }

  setPeltierMode(mode);
  peltierPulseUntilMs = millis() + pulseMs;
}

static void requestPeltierPulse(PeltierMode mode, unsigned long pulseMs) {
  if(mode != PELTIER_HEAT && mode != PELTIER_COOL) {
    applyPeltierCommand(mode, pulseMs);
    return;
  }

  unsigned long now = millis();
  bool reverseFromActive =
    (currentMode == PELTIER_HEAT && mode == PELTIER_COOL) ||
    (currentMode == PELTIER_COOL && mode == PELTIER_HEAT);

  if(reverseFromActive || pendingPeltierChange) {
    // Always force a 5-second OFF gap before switching heat <-> cool.
    setPeltierMode(PELTIER_OFF);
    peltierPulseUntilMs = 0;
    pendingPeltierChange = true;
    pendingPeltierMode = mode;
    pendingPeltierPulseMs = pulseMs;
    pendingPeltierApplyMs = now + PELTIER_SWITCH_GUARD_MS;
    Serial.println("Peltier switch delayed for thermal protection");
    return;
  }

  applyPeltierCommand(mode, pulseMs);
}

static void startWaterPumpPulse(unsigned long durationMs) {
  // The KB2040 code automatically handles the 5-second pulse, 
  // so we just need to send Command 1 (Trigger Pump)
  Wire1.beginTransmission(KB2040_I2C_ADDR);
  Wire1.write(1); 
  Wire1.endTransmission();
}
static void stopWaterPump() {
  // The KB2040 handles its own shutoff timer, but we will leave this 
  // function here so your processCommand logic doesn't break.
  Serial.println("Pump stop requested (Handled automatically by KB2040)");
}

static void initSensors() {
  Wire1.begin();

  if(sht4.begin(&Wire1)) {
    sht4Found = true;
    sht4.setHeater(SHT4X_NO_HEATER);
    Serial.println("SUCCESS: SHT41 Found on Qwiic (Wire1)!");
  } else {
    Serial.println("ERROR: SHT41 NOT found. Check Qwiic connection.");
  }

  Wire1.begin();
  if(veml.begin(&Wire1)) {
    vemlFound = true;
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    Serial.println("SUCCESS: VEML7700 Found!");
  } else {
    Serial.println("ERROR: VEML7700 NOT found. Check wiring.");
  }
}

static void readSensors(float lux) {
  if(sht4Found) {
    sensors_event_t humidity, temp;
    sht4.getEvent(&humidity, &temp);
    currentTempF = ((temp.temperature * 1.8f) + 32.0f) + TEMP_OFFSET_F;
    currentHumidity = humidity.relative_humidity;
  }

  currentLux = lux;

  // Request 2 bytes of moisture data from the KB2040
  Wire1.requestFrom((uint8_t)KB2040_I2C_ADDR, (uint8_t)2);
  if (Wire1.available() == 2) {
    byte msb = Wire1.read();
    byte lsb = Wire1.read();
    int rawSoil = (msb << 8) | lsb;

    // The KB2040 returns 1500 when simulating a dry override
    if(rawSoil == 1500) {
      currentSoil = 0.0f; // 1500 means artificially bone dry (0% moisture)
    } else if(rawSoil > 10) { // > 10 just ensures the sensor isn't totally disconnected/shorted
      
      // Map the calibrated values to a percentage (1020=Dry/0%, 600=Wet/100%)
      currentSoil = map(rawSoil, 1020, 600, 0, 100);
      
      // Constrain the result so it stays cleanly within 0.0% and 100.0%
      if(currentSoil < 0.0f) currentSoil = 0.0f;
      if(currentSoil > 100.0f) currentSoil = 100.0f;
      
    } else {
      currentSoil = 0.0f;
    }
  } else {
    currentSoil = -1.0f; // -1 means "Not wired / I2C Error"
  }
}

static void sendTelemetry() {
  unsigned long now = millis();
  
  // 1. Check if 10 seconds have passed. If not, stop here.
  if(now - lastTelemetryMs < TELEMETRY_INTERVAL_MS) return; 
  
  float lux = 0.0f;
  if(vemlFound) lux = veml.readLux();

  lastTelemetryMs = now;
  
  // 2. Gather the latest data from the sensors and I2C (KB2040)
  readSensors(lux); 

  // 3. Format the data into a single string packet
  String packet =
    "T:" + String(currentTempF, 1) +
    ",H:" + String(currentHumidity, 0) +
    ",L:" + String(currentLux, 1) +
    ",M:" + String(currentSoil, 0);
    
  // 4. THE UART PUSH: Send the packet over hardware TX/RX (Serial1)
  COMM_SERIAL.println(packet); 
  
  // (Optional) Also print it to the USB Serial Monitor so you can see it on your PC
  Serial.println("UART Packet: " + packet);
}

static void processCommand(const String &input) {
  String cmd = input;
  cmd.trim();
  if(cmd.length() == 0) return;

  // Peltier absolute overrides.
  if(cmd == "CMD:HEAT") {
    requestPeltierPulse(PELTIER_HEAT, PELTIER_PULSE_MS);
    return;
  }
  if(cmd == "CMD:COOL") {
    requestPeltierPulse(PELTIER_COOL, PELTIER_PULSE_MS);
    return;
  }
  if(cmd == "CMD:PELTIER_OFF") {
    applyPeltierCommand(PELTIER_OFF, 0);
    return;
  }
  if(cmd.equalsIgnoreCase("peltier auto")) {
    // ESP owns control logic; AUTO means stop manual forcing here.
    applyPeltierCommand(PELTIER_OFF, 0);
    return;
  }
  if(cmd == "CMD:HEAT_10S") {
    requestPeltierPulse(PELTIER_HEAT, PELTIER_PULSE_MS);
    return;
  }
  if(cmd == "CMD:COOL_10S") {
    requestPeltierPulse(PELTIER_COOL, PELTIER_PULSE_MS);
    return;
  }

  // Light commands.
  if(cmd.startsWith("CMD:LIGHT,")) {
    int pwmValue = cmd.substring(10).toInt();
    applyLightPwm(pwmValue);
    if(pwmValue > 0) {
      if(pwmValue < 120) lightLevel = 1;
      else if(pwmValue < 220) lightLevel = 2;
      else lightLevel = 3;
    }
    return;
  }
  if(cmd.equalsIgnoreCase("light on")) {
    applyLightPwm(pwmForLevel(lightLevel));
    return;
  }
  if(cmd.equalsIgnoreCase("light off")) {
    applyLightPwm(0);
    return;
  }
  if(cmd.equalsIgnoreCase("light auto")) {
    // ESP sends CMD:LIGHT,<pwm> in auto mode.
    return;
  }
  if(cmd.equalsIgnoreCase("lvl 1")) {
    lightLevel = 1;
    applyLightPwm(pwmForLevel(lightLevel));
    return;
  }
  if(cmd.equalsIgnoreCase("lvl 2")) {
    lightLevel = 2;
    applyLightPwm(pwmForLevel(lightLevel));
    return;
  }
  if(cmd.equalsIgnoreCase("lvl 3")) {
    lightLevel = 3;
    applyLightPwm(pwmForLevel(lightLevel));
    return;
  }

  // Mist absolute overrides.
  if(cmd.equalsIgnoreCase("mist on")) {
    setMist(true);
    return;
  }
  if(cmd.equalsIgnoreCase("mist off")) {
    setMist(false);
    return;
  }
  if(cmd.equalsIgnoreCase("mist")) {
    setMist(!mistOn);
    return;
  }
  if(cmd.equalsIgnoreCase("mist auto") || cmd.equalsIgnoreCase("humid auto")) {
    // ESP sends mist on/off in auto mode.
    return;
  }

  // Top fan commands.
  if(cmd.equalsIgnoreCase("fan on")) {
    setFan(true);
    return;
  }
  if(cmd.equalsIgnoreCase("fan off")) {
    setFan(false);
    return;
  }
  if(cmd.equalsIgnoreCase("fan")) {
    setFan(!fanOn);
    return;
  }

  // Bottom fan test commands.
  if(cmd.equalsIgnoreCase("bottom fan on")) {
    setBottomFan(true);
    return;
  }
  if(cmd.equalsIgnoreCase("bottom fan off")) {
    if(currentMode == PELTIER_OFF) setBottomFan(false);
    return;
  }
  if(cmd.equalsIgnoreCase("bottom fan")) {
    if(currentMode == PELTIER_OFF) setBottomFan(!bottomFanOn);
    return;
  }

  // Water pump commands.
  if(cmd == "CMD:WATER_PUMP") {
    startWaterPumpPulse(3000UL);
    return;
  }
  if(cmd == "CMD:WATER_PUMP_OFF") {
    stopWaterPump();
    return;
  }

  if(cmd == "CLEAN") {
    if(sht4Found) {
      sht4.setHeater(SHT4X_HIGH_HEATER_1S);
      sensors_event_t h, t;
      sht4.getEvent(&h, &t);
      sht4.setHeater(SHT4X_NO_HEATER);
      COMM_SERIAL.println("STATUS:CLEANING");
    }
    return;
  }

  // Optional bench simulation commands from USB or ESP:
  // SIM:T:72.5,H:55,L:400,S:35
  if(cmd.startsWith("SIM:")) {
    int tPos = cmd.indexOf("T:");
    int hPos = cmd.indexOf("H:");
    int lPos = cmd.indexOf("L:");
    int sPos = cmd.indexOf("S:");
    if(tPos >= 0) currentTempF = cmd.substring(tPos + 2).toFloat();
    if(hPos >= 0) currentHumidity = cmd.substring(hPos + 2).toFloat();
    if(lPos >= 0) currentLux = cmd.substring(lPos + 2).toFloat();
    if(sPos >= 0) currentSoil = cmd.substring(sPos + 2).toFloat();
    return;
  }
}

static void serviceActuators() {
  unsigned long now = millis();

  if(pendingPeltierChange && now >= pendingPeltierApplyMs) {
    applyPeltierCommand(pendingPeltierMode, pendingPeltierPulseMs);
  }

  if(currentMode == PELTIER_HEAT || currentMode == PELTIER_COOL) {
    if(peltierPulseUntilMs > 0 && now >= peltierPulseUntilMs) {
      applyPeltierCommand(PELTIER_OFF, 0);
    }

    if(currentMode != PELTIER_OFF) {
      if(peltierPwmTimer >= PWM_PERIOD_MS) peltierPwmTimer = 0;

      if(peltierPwmTimer < PWM_ON_MS) {
        // Wiring orientation requires this mapping: HEAT=TurnLeft, COOL=TurnRight.
        if(currentMode == PELTIER_HEAT) motorController.TurnLeft(255);
        else motorController.TurnRight(255);
      } else {
        motorController.Stop();
      }
    }
  }
  // waterPumpUntilMs logic removed! Handled by KB2040.
}

void setup() {
  Serial.begin(115200);
  COMM_SERIAL.begin(115200);
  while (!Serial) delay(10);

  motorController.Enable();
  motorController.Stop();

  pinMode(LIGHT_PWM_PIN, OUTPUT);
  pinMode(TOP_FAN_PIN, OUTPUT);
  pinMode(BOTTOM_FAN_PIN, OUTPUT);
  pinMode(MIST_PIN, OUTPUT); // <-- ADDED THIS BACK IN!

  // Initialize sensors and Wire1 FIRST
  initSensors(); 

  applyLightPwm(0);
  setFan(false);
  setBottomFan(false);
  setMist(false);
  setPeltierMode(PELTIER_OFF);
  
  Serial.println("System Online.");
  Serial.println("arduniofinal telemetry format: T/H/L/M");
  Serial.println("Type 'help' for commands.");
}

void loop() {
  if(Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    processCommand(cmd);
  }

  if(COMM_SERIAL.available()) {
    String cmd = COMM_SERIAL.readStringUntil('\n');
    processCommand(cmd);
  }

  serviceActuators();
  sendTelemetry();
}
