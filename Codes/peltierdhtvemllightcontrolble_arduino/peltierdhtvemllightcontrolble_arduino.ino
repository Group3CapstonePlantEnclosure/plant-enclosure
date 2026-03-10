#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <DHT.h>
#include <SoftwareSerial.h> // Added for Uno R3

// Setup SoftwareSerial for ESP32 communication
#define RX_PIN 10
#define TX_PIN 11
SoftwareSerial ESP_Serial(RX_PIN, TX_PIN);
#define COMM_SERIAL ESP_Serial 

// Hardware Pins
#define PELTIER_IN1 4
#define PELTIER_IN2 5
#define LIGHT_PWM_PIN 9
#define DHTPIN 7     // Digital pin for DHT11
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// ------------------- AUTO LIGHT CONTROL -------------------
float lowLuxThreshold  = 150.0;
float highLuxThreshold = 250.0;

int level = 3;
bool ledOn = false;

int pwmForLevel(int lvl) {
  if (lvl == 1) return 25;
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

bool dht_found = false; 
bool veml_found = false;

unsigned long lastUpdate = 0;
const long updateInterval = 10000;

void setup() {
  Serial.begin(115200);      // PC Serial Monitor
  COMM_SERIAL.begin(9600);   // ESP32 Communication (SoftwareSerial is best at 9600)

  pinMode(PELTIER_IN1, OUTPUT);
  pinMode(PELTIER_IN2, OUTPUT);
  pinMode(LIGHT_PWM_PIN, OUTPUT);

  applyPwm(0);

  // Initialize DHT sensor
  dht.begin();
  dht_found = true; 

  if (veml.begin()) {
    veml_found = true;
  } else {
    Serial.println("VEML7700 not found! Check wiring.");
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

  if (!dht_found) return;

  Serial.println("!!! DHT11 CLEANING CYCLE (Simulated - No physical heater) !!!");

  heaterStartTime = millis();
  heaterActive = true;
  lastHeaterCycle = millis();

  COMM_SERIAL.println("STATUS:CLEANING");

  Serial.println("Simulated blast complete. Cooling down for 10 seconds...");
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

  if (dht_found) {
    // Read humidity and temperature
    float humidity = dht.readHumidity();
    float tempC = dht.readTemperature(); 

    if (isnan(humidity) || isnan(tempC)) {
      Serial.println("Failed to read from DHT sensor!");
    } else {
      t = ((tempC * 1.8) + 32.0) + tempOffset;
      h = humidity;
    }
  }

  String dataPacket =
    "T:" + String(t,1) +
    ",H:" + String(h,0) +
    ",L:" + String(lux,1);

  COMM_SERIAL.println(dataPacket);

  Serial.println("UART Packet: " + dataPacket);
}