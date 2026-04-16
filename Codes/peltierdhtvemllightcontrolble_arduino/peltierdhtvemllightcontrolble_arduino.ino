#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include "Adafruit_SHT4x.h"

#define COMM_SERIAL Serial1 

// Hardware Pins
#define PELTIER_IN1 4
#define PELTIER_IN2 5
#define LIGHT_PWM_PIN 9
const int phPin = A0;          // The analog pin connected to Po [cite: 50, 51]

Adafruit_SHT4x sht4 = Adafruit_SHT4x();
Adafruit_VEML7700 veml = Adafruit_VEML7700(); [cite: 2]

// ------------------- AUTO LIGHT CONTROL -------------------
float lowLuxThreshold  = 150.0;
float highLuxThreshold = 250.0;
int level = 3; [cite: 3]
bool ledOn = false;

int pwmForLevel(int lvl) {
  if (lvl == 1) return 25;
  if (lvl == 2) return 100; [cite: 4]
  return 255;
}

void applyPwm(int pwm) {
  analogWrite(LIGHT_PWM_PIN, constrain(pwm, 0, 255));
} [cite: 5]

void turnOn() {
  int pwm = pwmForLevel(level);
  applyPwm(pwm);
  ledOn = true;

  Serial.print("LED ON | Lux below ");
  Serial.println(lowLuxThreshold);
} [cite: 6]

void turnOff() {
  applyPwm(0);
  ledOn = false;

  Serial.print("LED OFF | Lux above ");
  Serial.println(highLuxThreshold);
} [cite: 7]

// ------------------- PH SENSOR VARIABLES -------------------
float calibrationOffset = -3.71; // Adjust this value to calibrate the sensor [cite: 51, 52]

// Variables for averaging the readings
const int numReadings = 10;
int readings[numReadings]; [cite: 52]
int readIndex = 0;              
long total = 0;                  
int average = 0; [cite: 53]                
float phValue = 0.0; // Added as global so sendSensorData can access it
unsigned long lastPhUpdate = 0;
const long phInterval = 500; // Replacing delay(500) with a non-blocking timer

// ------------------- SERIAL COMMANDS -----------------------
void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return; [cite: 7]

  if (cmd == "help") { [cite: 8]
    Serial.println("Commands:");
    Serial.println("  low <lux>");
    Serial.println("  high <lux>"); [cite: 8]
    Serial.println("  lvl <1|2|3>"); [cite: 9]
    Serial.println("  show");
    Serial.println("  CLEAN");
    return;
  } [cite: 10]

  if (cmd == "show") {
    Serial.print("Low threshold: ");
    Serial.println(lowLuxThreshold);
    Serial.print("High threshold: ");
    Serial.println(highLuxThreshold);
    Serial.print("Level: ");
    Serial.println(level); [cite: 10, 11]
    Serial.print("PWM: ");
    Serial.println(pwmForLevel(level));
    return;
  }

  if (cmd == "CLEAN") {
    startCleaningCycle();
    return;
  } [cite: 11, 12]

  int sp = cmd.indexOf(' ');
  String key = (sp == -1) ? cmd : cmd.substring(0, sp); [cite: 12]
  String val = (sp == -1) ? ""  : cmd.substring(sp + 1); [cite: 13]

  key.trim();
  val.trim();

  if (key == "low") { [cite: 14]
    float v = val.toFloat();
    if (v >= 0) { [cite: 15]
      lowLuxThreshold = v;
      Serial.print("Low threshold set to ");
      Serial.println(lowLuxThreshold);
    } [cite: 15, 16]
  }

  else if (key == "high") {
    float v = val.toFloat();
    if (v >= 0) { [cite: 16, 17]
      highLuxThreshold = v;
      Serial.print("High threshold set to ");
      Serial.println(highLuxThreshold);
    } [cite: 17, 18]
  }

  else if (key == "lvl") {
    int lv = val.toInt();
    if (lv >= 1 && lv <= 3) { [cite: 18, 19]
      level = lv;
      Serial.print("Level set to "); [cite: 20]
      Serial.println(level);

      if (ledOn) {
        applyPwm(pwmForLevel(level));
      } [cite: 20, 21]
    }
  }

  else {
    Serial.println("Unknown command. Type: help");
  } [cite: 21, 22]
}
// -----------------------------------------------------------


// Calibration & Maintenance
float tempOffset = -7.5;
unsigned long lastHeaterCycle = 0;
const unsigned long heaterInterval = 900000;
bool heaterActive = false; [cite: 22, 23]
unsigned long heaterStartTime = 0;

bool sht4_found = false, veml_found = false;
unsigned long lastUpdate = 0; [cite: 23, 24]
const long updateInterval = 10000;

void setup() {
  Serial.begin(115200); // Set to 115200 to accommodate both codes 
  COMM_SERIAL.begin(115200);
  // Using hardware Serial1 on the R4! [cite: 24, 25]

  pinMode(PELTIER_IN1, OUTPUT);
  pinMode(PELTIER_IN2, OUTPUT);
  pinMode(LIGHT_PWM_PIN, OUTPUT);

  applyPwm(0);

  // Initialize the array for averaging
  for (int i = 0; i < numReadings; i++) {
    readings[i] = 0;
  } [cite: 54, 55]

  // Initialize the secondary I2C bus for the Qwiic connector [cite: 26]
  Wire1.begin();
  // Tell the SHT4x library to use Wire1 instead of the default Wire [cite: 26, 27]
  if (sht4.begin(&Wire1)) {
    sht4_found = true;
    sht4.setHeater(SHT4X_NO_HEATER); [cite: 27, 28]
    Serial.println("SUCCESS: SHT41 Found on Qwiic (Wire1)!");
  } else {
    Serial.println("ERROR: SHT41 NOT found. Check Qwiic connection.");
  } [cite: 28, 29]

  // Initialize the primary I2C bus for standard pins
  Wire.begin();
  // If your VEML7700 is wired to pins A4/A5, use veml.begin() [cite: 29, 30]
  // NOTE: If you daisy-chained the VEML7700 to the Qwiic connector instead, 
  // you must change this to: veml.begin(&Wire1)
  if (veml.begin()) {
    veml_found = true;
    Serial.println("SUCCESS: VEML7700 Found!"); [cite: 30, 31]
  } else {
    Serial.println("ERROR: VEML7700 NOT found. Check wiring.");
  }

  Serial.println("pH Sensor Initialized."); [cite: 55]
  Serial.println("System Online.");
} [cite: 31, 32]

void loop() {
  unsigned long currentMillis = millis();

  // Handle PC serial commands
  handleSerial();

  // --- PH SENSOR LOGIC (Every 500ms, non-blocking) ---
  if (currentMillis - lastPhUpdate >= phInterval) {
    lastPhUpdate = currentMillis;

    // Subtract the last reading and add the new one for a rolling average
    total = total - readings[readIndex];
    readings[readIndex] = analogRead(phPin); [cite: 55, 56]
    total = total + readings[readIndex];
    readIndex = readIndex + 1;

    if (readIndex >= numReadings) { [cite: 56, 57]
      readIndex = 0;
    }

    average = total / numReadings;
    
    // Convert the 10-bit analog reading (0-1023) to a voltage (0-5V) [cite: 57, 58]
    float voltage = average * (5.0 / 1023.0);
    
    // Typical conversion formula for these modules: pH = 3.5 * voltage + offset [cite: 58, 59]
    phValue = 3.5 * voltage + calibrationOffset;

    // Optional: Print to Serial every 500ms like the original script. 
    // Comment this block out if it clutters your command menu too much.
    Serial.print("Analog: "); [cite: 59, 60]
    Serial.print(average);
    Serial.print(" | Voltage: ");
    Serial.print(voltage, 3);
    Serial.print(" V | pH: ");
    Serial.println(phValue, 2); [cite: 60]
  }

  // --- Automatic Hourly Cleaning --- [cite: 32, 33]
  if (currentMillis - lastHeaterCycle >= heaterInterval && !heaterActive) {
    startCleaningCycle();
  } [cite: 33, 34]

  // --- Heater Cooldown ---
  if (heaterActive && (currentMillis - heaterStartTime > 10000)) {
    Serial.println("Cooldown finished. Resuming accurate data.");
    heaterActive = false; [cite: 34, 35]
  }

  if (!heaterActive) {

    // Listen for ESP32 Commands
    if (COMM_SERIAL.available()) {
      String received = COMM_SERIAL.readStringUntil('\n');
      received.trim(); [cite: 35, 36]
      processESP32Command(received);
    }

    // Sensor update
    if (currentMillis - lastUpdate >= updateInterval) {

      lastUpdate = currentMillis;
      float lux = 0; [cite: 36, 37]
      if (veml_found) lux = veml.readLux();

      // -------- AUTO LIGHT CONTROL --------
      if (!ledOn && lux <= lowLuxThreshold) {
        turnOn();
      } [cite: 37, 38]

      if (ledOn && lux >= highLuxThreshold) {
        turnOff();
      } [cite: 38, 39]
      // ------------------------------------

      sendSensorData(lux);
    } [cite: 39, 40]
  }
}

void startCleaningCycle() {

  if (!sht4_found) return;

  Serial.println("!!! SHT4x CLEANING START (1s Blast) !!!");

  sht4.setHeater(SHT4X_HIGH_HEATER_1S);

  sensors_event_t h, t;
  sht4.getEvent(&h, &t); [cite: 40, 41]

  sht4.setHeater(SHT4X_NO_HEATER);

  heaterStartTime = millis();
  heaterActive = true;
  lastHeaterCycle = millis();

  COMM_SERIAL.println("STATUS:CLEANING");

  Serial.println("Blast complete. Cooling down for 10 seconds...");
} [cite: 41, 42]

void processESP32Command(String cmd) {

  if (cmd == "CMD:COOL") {
    digitalWrite(PELTIER_IN1, HIGH);
    digitalWrite(PELTIER_IN2, LOW);
  } [cite: 42, 43]

  else if (cmd == "CMD:HEAT") {
    digitalWrite(PELTIER_IN1, LOW);
    digitalWrite(PELTIER_IN2, HIGH);
  } [cite: 43, 44]

  else if (cmd == "CMD:PELTIER_OFF") {
    digitalWrite(PELTIER_IN1, LOW);
    digitalWrite(PELTIER_IN2, LOW);
  } [cite: 44, 45]

  else if (cmd.startsWith("CMD:LIGHT,")) {
    int pwmValue = cmd.substring(10).toInt();
    analogWrite(LIGHT_PWM_PIN, pwmValue);
  } [cite: 45, 46]
}

void sendSensorData(float lux) {

  float t = 0.0;
  float h = 0.0;
  if (sht4_found) { [cite: 46, 47]
    sensors_event_t humidity, temp;
    sht4.getEvent(&humidity, &temp);

    t = ((temp.temperature * 1.8) + 32.0) + tempOffset;
    h = humidity.relative_humidity; [cite: 47, 48]
  }

  String dataPacket =
    "T:" + String(t,1) +
    ",H:" + String(h,0) +
    ",L:" + String(lux,1) + 
    ",P:" + String(phValue,2); // Added pH to the UART string [cite: 48, 49]

  COMM_SERIAL.println(dataPacket); [cite: 49]

  Serial.println("UART Packet: " + dataPacket);
}
