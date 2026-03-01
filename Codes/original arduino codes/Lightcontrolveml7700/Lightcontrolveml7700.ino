#include <Wire.h>
#include <Adafruit_VEML7700.h>

Adafruit_VEML7700 veml;

const int LED_GATE_PIN = 9; // PWM pin

// ---- User settings ----
float lowLuxThreshold  = 150.0;  // Turn ON below this
float highLuxThreshold = 250.0;  // Turn OFF above this

// ---- Brightness levels ----
int level = 3;  // default level

int pwmForLevel(int lvl) {
  if (lvl == 1) return 25;  // Low
  if (lvl == 2) return 150;  // Mid
  return 255;                // Full brightness
}

// ---- State ----
bool ledOn = false;

void applyPwm(int pwm) {
  analogWrite(LED_GATE_PIN, constrain(pwm, 0, 255));
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
    Serial.println("  low <lux>   -> turn ON below this");
    Serial.println("  high <lux>  -> turn OFF above this");
    Serial.println("  lvl <1|2|3> -> brightness level");
    Serial.println("  show        -> show settings");
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

void setup() {
  pinMode(LED_GATE_PIN, OUTPUT);
  applyPwm(0);

  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin();
  if (!veml.begin()) {
    Serial.println("ERROR: VEML7700 not found.");
    while (1);
  }

  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("Ready.");
  Serial.println("Low lux -> ON | High lux -> OFF");
}

void loop() {
  handleSerial();

  float lux = veml.readLux();

  if (!ledOn && lux <= lowLuxThreshold) {
    turnOn();
  }

  if (ledOn && lux >= highLuxThreshold) {
    turnOff();
  }

  delay(100);
}