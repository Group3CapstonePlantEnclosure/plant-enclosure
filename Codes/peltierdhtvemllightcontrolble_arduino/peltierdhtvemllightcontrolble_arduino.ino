#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include "Adafruit_SHT4x.h"
#include <BTS7960.h>
#include <elapsedMillis.h>

// ------------------- COMMUNICATION -------------------
// Hardwired UART connection using TXD2/RXD2
#define COMM_SERIAL Serial2 

// ------------------- HARDWARE PINS -------------------
#define LIGHT_PWM_PIN 9
// --- Peltier & Fan Pins ---
const uint8_t R_EN = 8;
const uint8_t L_EN = 7; // Pin 7 to avoid conflict with LIGHT_PWM_PIN
const uint8_t RPWM = 10;
const uint8_t LPWM = 11;
// --- Humidifier Pins ---
const int mistPin = 4; // Changed from 8 to 4 to prevent conflict with R_EN
bool mistIsOn = false;
const uint8_t TOP_FAN_PIN = 5;    // Replaced old PELTIER_IN2
const uint8_t BOTTOM_FAN_PIN = 6; 

// ------------------- OBJECTS -------------------------
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Adafruit_VEML7700 veml = Adafruit_VEML7700();
BTS7960 motorController(L_EN, R_EN, LPWM, RPWM);

// ------------------- PELTIER VARIABLES ---------------
elapsedMillis slowPwmTimer;
const unsigned long PWM_PERIOD = 2000; // 2-second cycle
const float DUTY_CYCLE = 0.83; // 83% duty cycle to limit to ~5A
const unsigned long ON_TIME = PWM_PERIOD * DUTY_CYCLE;

enum Mode { COOLING, HEATING, OFF };
Mode currentMode = OFF;

// ------------------- LIGHT CONTROL VARIABLES ---------
float lowLuxThreshold  = 150.0;  
float highLuxThreshold = 250.0;
int level = 3;

bool autoMode = true;
bool ledOn = false;

// ------------------- PH SENSOR VARIABLES -------------
float calibrationOffset = -3.71;
const int numReadings = 10;
int readings[numReadings];
int readIndex = 0;              
long total = 0;
int average = 0;

float phValue = 0.0;
unsigned long lastPhUpdate = 0;
const long phInterval = 500;

// ------------------- SHT4x & SYSTEM VARIABLES --------
float tempOffset = -7.5;
unsigned long lastHeaterCycle = 0;
const unsigned long heaterInterval = 900000;
bool heaterActive = false;
unsigned long heaterStartTime = 0;

bool sht4_found = false;
bool veml_found = false;
unsigned long lastUpdate = 0;
const long updateInterval = 10000;

// ====================================================================
// ==================== PELTIER & FAN FUNCTIONS =======================
// ====================================================================

void applySafetyPause() {
  Serial.println("! THERMAL SHOCK PREVENTION !");
  Serial.println("Waiting 5 seconds for temperatures to neutralize...");
  motorController.Stop();
  delay(5000); 
  Serial.println("Resuming...");
}

void applyStartupDelay() {
  Serial.println("Applying startup delay (5 seconds) to stabilize power...");
  motorController.Stop();
  analogWrite(TOP_FAN_PIN, 0);
  analogWrite(BOTTOM_FAN_PIN, 0);
  delay(5000);
  Serial.println("Starting...");
}

void setPeltierMode(Mode newMode) {
  if (currentMode == newMode) return;

  if (newMode == HEATING) {
    if (currentMode == COOLING) applySafetyPause();
    else if (currentMode == OFF) applyStartupDelay();

    currentMode = HEATING;
    slowPwmTimer = 0; 
    analogWrite(TOP_FAN_PIN, 255);    // Large top fan FULL BLAST
    analogWrite(BOTTOM_FAN_PIN, 255);  // Small bottom fan SLOW
    Serial.println(">>> Peltier Mode: HEATING");

  } else if (newMode == COOLING) {
    if (currentMode == HEATING) applySafetyPause();
    else if (currentMode == OFF) applyStartupDelay();
    
    currentMode = COOLING;
    slowPwmTimer = 0; 
    analogWrite(TOP_FAN_PIN, 255);    // Large top fan FULL BLAST
    analogWrite(BOTTOM_FAN_PIN, 255);  // Small bottom fan SLOW
    Serial.println(">>> Peltier Mode: COOLING");

  } else if (newMode == OFF) {
    currentMode = OFF;
    analogWrite(TOP_FAN_PIN, 0);
    analogWrite(BOTTOM_FAN_PIN, 0);
    motorController.Stop();
    Serial.println(">>> Peltier Mode: OFF");
  }
}

// ====================================================================
// ==================== LIGHT CONTROL FUNCTIONS =======================
// ====================================================================

int pwmForLevel(int lvl) {
  if (lvl == 1) return 80;
  if (lvl == 2) return 160;   
  return 255;
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

  // Added Single Character Overrides from Peltier testing sketch
  if (cmd == "H" || cmd == "h") { setPeltierMode(HEATING); return; }
  if (cmd == "C" || cmd == "c") { setPeltierMode(COOLING); return; }
  if (cmd == "O" || cmd == "o") { setPeltierMode(OFF); return; }

  // Added Humidifier Command
  if (cmd.equalsIgnoreCase("mist")) {
    mistIsOn = !mistIsOn; // Flip the state
    digitalWrite(mistPin, mistIsOn ? HIGH : LOW);
    Serial.print("Mister Status: ");
    Serial.println(mistIsOn ? "ON" : "OFF");
    return;
  }

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
    Serial.println("  H/C/O       -> Manual Peltier override (Heat, Cool, Off)");
    Serial.println("  mist        -> Toggle Humidifier ON/OFF");
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
    setPeltierMode(COOLING);
  } 
  else if (cmd == "CMD:HEAT") {
    setPeltierMode(HEATING);
  } 
  else if (cmd == "CMD:PELTIER_OFF") {
    setPeltierMode(OFF);
  } 
  else if (cmd.startsWith("CMD:LIGHT,")) {
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

  // Pack the string without the pH data
  String dataPacket =
    "T:" + String(t,1) +
    ",H:" + String(h,0) +
    ",L:" + String(lux,1); 

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

  // Initialize Peltier & Fans
  motorController.Enable();
  motorController.Stop();
  pinMode(TOP_FAN_PIN, OUTPUT);
  pinMode(BOTTOM_FAN_PIN, OUTPUT);
  analogWrite(TOP_FAN_PIN, 0);
  analogWrite(BOTTOM_FAN_PIN, 0);

  // Initialize Light
  pinMode(LIGHT_PWM_PIN, OUTPUT);
  applyPwm(0);

  // Initialize Humidifier (Mister)
  pinMode(mistPin, OUTPUT);
  digitalWrite(mistPin, LOW); // Start with the mister OFF

  // Initialize Sensors
  Wire1.begin();
  if (sht4.begin(&Wire1)) {
    sht4_found = true;
    sht4.setHeater(SHT4X_NO_HEATER);
    Serial.println("SUCCESS: SHT41 Found on Qwiic (Wire1)!");
  } else {
    Serial.println("ERROR: SHT41 NOT found. Check Qwiic connection.");
  }
  
  Wire.begin();
  if (veml.begin()) {
    veml_found = true;
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    Serial.println("SUCCESS: VEML7700 Found!");
  } else {
    Serial.println("ERROR: VEML7700 NOT found. Check wiring.");
  }

  Serial.println("System Online.");
  Serial.println("AUTO mode uses sensor thresholds.");
  Serial.println("Type 'help' for commands or H/C/O to control Peltier.");
}

// ====================================================================
// ==================== MAIN LOOP =====================================
// ====================================================================

void loop() {
  unsigned long currentMillis = millis();

  // 1. Handle PC serial commands
  handleSerial();

  // 2. Slow PWM Logic for Peltier
  if (currentMode != OFF) {
    if (slowPwmTimer >= PWM_PERIOD) {
      slowPwmTimer = 0;
    }

    if (slowPwmTimer < ON_TIME) {
      if (currentMode == HEATING) motorController.TurnRight(255);
      else if (currentMode == COOLING) motorController.TurnLeft(255);
    } else {
      motorController.Stop();
    }
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
