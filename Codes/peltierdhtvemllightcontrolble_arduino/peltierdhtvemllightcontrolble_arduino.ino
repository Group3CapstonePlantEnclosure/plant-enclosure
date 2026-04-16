#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include "Adafruit_SHT4x.h"

#define COMM_SERIAL Serial1 // Using hardware Serial1 for ESP32

// ------------------- HARDWARE PINS -------------------
#define PELTIER_IN1 4
#define PELTIER_IN2 5
#define LIGHT_PWM_PIN 9
const int phPin = A0;

// ------------------- OBJECTS -------------------------
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// ------------------- LIGHT CONTROL VARIABLES ---------
float lowLuxThreshold  = 150.0;  // Turn ON below this
float highLuxThreshold = 250.0;  // Turn OFF above this
int level = 3;                   // Default level
bool autoMode = true;            // Sensor controls light by default
bool ledOn = false;

// ------------------- PH SENSOR VARIABLES -------------
float calibrationOffset = -3.71; // Adjust to calibrate
const int numReadings = 10;
int readings[numReadings];
int readIndex = 0;              
long total = 0;                  
int average = 0;                
float phValue = 0.0;
unsigned long lastPhUpdate = 0;
const long phInterval = 500;     // Non-blocking 500ms timer

// ------------------- SHT4x & SYSTEM VARIABLES --------
float tempOffset = -7.5;
unsigned long lastHeaterCycle = 0;
const unsigned long heaterInterval = 900000;
bool heaterActive = false;
unsigned long heaterStartTime = 0;

bool sht4_found = false;
bool veml_found = false;
unsigned long lastUpdate = 0;
const long updateInterval = 10000; // 10s telemetry update

// ====================================================================
// ==================== LIGHT CONTROL FUNCTIONS =======================
// ====================================================================

int pwmForLevel(int lvl) {
  if (lvl == 1) return 80;    // Low
  if (lvl == 2) return 160;   // Mid
  return 255;                 // Full brightness
}

void applyPwm(int pwm) {
  analogWrite(LIGHT_PWM_PIN, constrain(pwm, 0, 255));
}

void turnOn() {
  int pwm = pwmForLevel(level);
  applyPwm(pwm);
  ledOn = true;

  Serial.print("LED ON | Level ");
  Serial.print(level);
  Serial.print(" | PWM ");
  Serial.print(pwm);
  Serial.print(" | Lux = ");
  if (veml_found) Serial.println(veml.readLux(), 1);
  else Serial.println("N/A");
}

void turnOff() {
  applyPwm(0);
  ledOn = false;
  
  Serial.print("LED OFF | Lux = ");
  if (veml_found) Serial.println(veml.readLux(), 1);
  else Serial.println("N/A");
}

void printStatus() {
  Serial.print("Mode: ");
  Serial.println(autoMode ? "AUTO" : "MANUAL");

  Serial.print("Low threshold: ");
  Serial.println(lowLuxThreshold);

  Serial.print("High threshold: ");
  Serial.println(highLuxThreshold);

  Serial.print("Level: ");
  Serial.println(level);

  Serial.print("PWM: ");
  Serial.println(pwmForLevel(level));

  Serial.print("LED state: ");
  Serial.println(ledOn ? "ON" : "OFF");

  Serial.print("Current lux: ");
  if (veml_found) Serial.println(veml.readLux(), 1);
  else Serial.println("N/A");
}

// ====================================================================
// ==================== SERIAL COMMAND HANDLER ========================
// ====================================================================

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    Serial.println("Commands:");
    Serial.println("  low <lux>   -> AUTO: turn ON below this");
    Serial.println("  high <lux>  -> AUTO: turn OFF above this");
    Serial.println("  lvl <1|2|3> -> set brightness level");
    Serial.println("  auto        -> sensor controls light");
    Serial.println("  on          -> manual ON");
    Serial.println("  off         -> manual OFF");
    Serial.println("  lux         -> print current lux");
    Serial.println("  show        -> show settings");
    Serial.println("  CLEAN       -> trigger SHT4x heater cycle");
    return;
  }

  if (cmd == "show") {
    printStatus();
    return;
  }

  if (cmd == "CLEAN") {
    startCleaningCycle();
    return;
  }

  if (cmd == "lux") {
    Serial.print("Lux: ");
    if (veml_found) Serial.println(veml.readLux(), 1);
    else Serial.println("N/A");
    return;
  }

  if (cmd == "auto") {
    autoMode = true;
    Serial.println("Mode set to AUTO");
    return;
  }

  if (cmd == "on") {
    autoMode = false;
    turnOn();
    Serial.println("Manual mode enabled");
    return;
  }

  if (cmd == "off") {
    autoMode = false;
    turnOff();
    Serial.println("Manual mode enabled");
    return;
  }

  int sp = cmd.indexOf(' ');
  String key = (sp == -1) ? cmd : cmd.substring(0, sp);
  String val = (sp == -1) ? "" : cmd.substring(sp + 1);
  key.trim();
  val.trim();

  if (key == "low") {
    float v = val.toFloat();
    if (v >= 0) {
      lowLuxThreshold = v;
      Serial.print("Low threshold set to ");
      Serial.println(lowLuxThreshold);
    } else {
      Serial.println("Usage: low <lux>");
    }
  }
  else if (key == "high") {
    float v = val.toFloat();
    if (v >= 0) {
      highLuxThreshold = v;
      Serial.print("High threshold set to ");
      Serial.println(highLuxThreshold);
    } else {
      Serial.println("Usage: high <lux>");
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
        Serial.print("Applied PWM ");
        Serial.println(pwmForLevel(level));
      }
    } else {
      Serial.println("Usage: lvl 1, 2, or 3");
    }
  }
  else {
    Serial.println("Unknown command. Type: help");
  }
}

// ====================================================================
// ==================== MAINTENANCE & ESP32 COMMS =====================
// ====================================================================

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
    // Disable auto mode if ESP manually commands the light
    autoMode = false; 
    int pwmValue = cmd.substring(10).toInt();
    applyPwm(pwmValue);
    ledOn = (pwmValue > 0);
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
    ",L:" + String(lux,1) + 
    ",P:" + String(phValue,2);

  COMM_SERIAL.println(dataPacket);
  Serial.println("UART Packet: " + dataPacket);
}

// ====================================================================
// ==================== SETUP =========================================
// ====================================================================

void setup() {
  Serial.begin(115200); 
  COMM_SERIAL.begin(115200);
  while (!Serial) delay(10);

  pinMode(PELTIER_IN1, OUTPUT);
  pinMode(PELTIER_IN2, OUTPUT);
  pinMode(LIGHT_PWM_PIN, OUTPUT);
  applyPwm(0);

  // Initialize the array for averaging
  for (int i = 0; i < numReadings; i++) {
    readings[i] = 0;
  }

  // Initialize the secondary I2C bus for the Qwiic connector
  Wire1.begin();
  if (sht4.begin(&Wire1)) {
    sht4_found = true;
    sht4.setHeater(SHT4X_NO_HEATER);
    Serial.println("SUCCESS: SHT41 Found on Qwiic (Wire1)!");
  } else {
    Serial.println("ERROR: SHT41 NOT found. Check Qwiic connection.");
  }

  // Initialize the primary I2C bus for standard pins
  Wire.begin();
  if (veml.begin()) {
    veml_found = true;
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    Serial.println("SUCCESS: VEML7700 Found!");
  } else {
    Serial.println("ERROR: VEML7700 NOT found. Check wiring.");
  }

  Serial.println("pH Sensor Initialized.");
  Serial.println("System Online.");
  Serial.println("AUTO mode uses sensor thresholds.");
  Serial.println("Type 'help' for commands.");
}

// ====================================================================
// ==================== MAIN LOOP =====================================
// ====================================================================

void loop() {
  unsigned long currentMillis = millis();

  // 1. Handle PC serial commands
  handleSerial();

  // 2. PH SENSOR LOGIC (Every 500ms, non-blocking)
  if (currentMillis - lastPhUpdate >= phInterval) {
    lastPhUpdate = currentMillis;

    // Rolling average logic
    total = total - readings[readIndex];
    readings[readIndex] = analogRead(phPin);
    total = total + readings[readIndex];
    readIndex = readIndex + 1;

    if (readIndex >= numReadings) {
      readIndex = 0;
    }

    average = total / numReadings;
    
    // Conversion formulas
    float voltage = average * (5.0 / 1023.0);
    phValue = 3.5 * voltage + calibrationOffset;

    // Print to Serial every 500ms
    Serial.print("Analog: ");
    Serial.print(average);
    Serial.print(" | Voltage: ");
    Serial.print(voltage, 3);
    Serial.print(" V | pH: ");
    Serial.println(phValue, 2);
  }

  // 3. Automatic Hourly Cleaning
  if (currentMillis - lastHeaterCycle >= heaterInterval && !heaterActive) {
    startCleaningCycle();
  }

  // 4. Heater Cooldown
  if (heaterActive && (currentMillis - heaterStartTime > 10000)) {
    Serial.println("Cooldown finished. Resuming accurate data.");
    heaterActive = false;
  }

  // 5. General Logic (Skipped if heating/cooling down)
  if (!heaterActive) {
    
    // Listen for ESP32 Commands
    if (COMM_SERIAL.available()) {
      String received = COMM_SERIAL.readStringUntil('\n');
      received.trim();
      processESP32Command(received);
    }

    // Auto Light Control Logic
    float lux = 0;
    if (veml_found) {
      lux = veml.readLux();
    }
    
    // Only adjust based on sensor if the system is in Auto Mode
    if (autoMode) {
      if (!ledOn && lux <= lowLuxThreshold) {
        turnOn();
      }
      if (ledOn && lux >= highLuxThreshold) {
        turnOff();
      }
    }

    // Sensor update to ESP32 every 10 seconds
    if (currentMillis - lastUpdate >= updateInterval) {
      lastUpdate = currentMillis;
      sendSensorData(lux);
    }
  }
}
