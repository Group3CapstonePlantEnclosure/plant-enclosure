#include <Wire.h>
#include <Adafruit_VEML7700.h>

Adafruit_VEML7700 veml;

const int LED_GATE_PIN = 9;  // PWM pin

// ---- User settings ----
float lowLuxThreshold  = 150.0;  // Turn ON below this
float highLuxThreshold = 250.0;  // Turn OFF above this

// ---- Brightness levels ----
int level = 3;  // default level
bool autoMode = true;
bool ledOn = false;

int pwmForLevel(int lvl) {
  if (lvl == 1) return 80;    // Low
  if (lvl == 2) return 160;   // Mid
  return 255;                 // Full brightness
}

void applyPwm(int pwm) {
  analogWrite(LED_GATE_PIN, constrain(pwm, 0, 255));
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
  Serial.println(veml.readLux(), 1);
}

void turnOff() {
  applyPwm(0);
  ledOn = false;

  Serial.print("LED OFF | Lux = ");
  Serial.println(veml.readLux(), 1);
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
  Serial.println(veml.readLux(), 1);
}

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
    return;
  }

  if (cmd == "show") {
    printStatus();
    return;
  }

  if (cmd == "lux") {
    Serial.print("Lux: ");
    Serial.println(veml.readLux(), 1);
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

void setup() {
  pinMode(LED_GATE_PIN, OUTPUT);
  applyPwm(0);

  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin();

  if (!veml.begin()) {
    Serial.println("ERROR: VEML7700 not found.");
    while (1) delay(10);
  }

  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("Ready.");
  Serial.println("AUTO mode uses sensor thresholds.");
  Serial.println("Type 'help' for commands.");
}

void loop() {
  handleSerial();

  if (autoMode) {
    float lux = veml.readLux();

    if (!ledOn && lux <= lowLuxThreshold) {
      turnOn();
    }

    if (ledOn && lux >= highLuxThreshold) {
      turnOff();
    }
  }

  delay(100);
}
