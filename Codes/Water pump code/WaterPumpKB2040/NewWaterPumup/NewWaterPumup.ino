/**
 * Water Pump System for Adafruit KB2040 (I2C Peripheral Edition)
 * Operates entirely on a 0-100% Moisture Scale
 * 0% = Dry (1020 raw), 100% = Wet (600 raw)
 */

#include <Wire.h>

#define I2C_ADDR 0x08 // KB2040 I2C Address

int sensorPin = A0;
int relayPin = 7;
int mistPin = 8;
int buttonPin = 9;

// Threshold Logic (NOW IN PERCENTAGES)
int threshold = 12;   // Target moisture is 20%
int hysteresis = 5;   // Buffer: ON if < 15%, OFF if > 25%
bool pumpIsOn = false;

// Override variables
bool simulatedOverrideActive = false;
unsigned long simulatedOverrideStart = 0;
const unsigned long simulatedOverrideDuration = 5000; 

// Peripheral states
bool arduinoMistCommand = false;
bool buttonHandled = false;

// Global tracker for moisture percentage to send over I2C
int currentMoisturePct = 0;

void setup() {
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); // Relay Setup (Active-Low: HIGH is OFF)

  pinMode(mistPin, OUTPUT);
  digitalWrite(mistPin, LOW);

  pinMode(buttonPin, INPUT_PULLUP);

  Serial.begin(115200);

  Wire.begin(I2C_ADDR);
  Wire.onReceive(receiveEvent); 
  Wire.onRequest(requestEvent); 
  
  Serial.println("KB2040 I2C Water Pump Controller Online (Percentage Mode)");
}

void loop() {
  checkSerialMonitor();
  checkButton();
  updatePumpState();
  updateMistState();
}

void trigger5SecondOverride() {
  simulatedOverrideActive = true;
  simulatedOverrideStart = millis();
  Serial.println("--- Manual Override: Forcing Pump ON ---");
}

void checkSerialMonitor() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.equalsIgnoreCase("on")) trigger5SecondOverride();
  }
}

void checkButton() {
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH;
  static bool currentButtonState = HIGH;
  bool reading = digitalRead(buttonPin);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > 50) {
    if (reading != currentButtonState) {
      currentButtonState = reading;
      if (currentButtonState == LOW && !buttonHandled) {
        trigger5SecondOverride();
        buttonHandled = true;
      }
    }
  }
  
  if (currentButtonState == HIGH) {
    buttonHandled = false;
  }
  lastButtonState = reading;
}

void updateMistState() {
  digitalWrite(mistPin, arduinoMistCommand ? HIGH : LOW);
}

void updatePumpState() {
  int rawValue = analogRead(sensorPin);
  
  // Convert Raw Value to 0-100%
  int mappedPct = map(rawValue, 1020, 600, 0, 100);
  mappedPct = constrain(mappedPct, 0, 100);

  // Handle the Override (Force to 0% to trigger pump)
  if (simulatedOverrideActive) {
    if (millis() - simulatedOverrideStart < simulatedOverrideDuration) {
      mappedPct = 0; 
    } else {
      simulatedOverrideActive = false;
      Serial.println("--- Override Complete ---");
    }
  }

  currentMoisturePct = mappedPct;

  // --- PERCENTAGE HYSTERESIS LOGIC ---
  // Turn ON if it gets too dry (below threshold - buffer)
  if (currentMoisturePct <= (threshold - hysteresis)) {
    pumpIsOn = true;
  } 
  // Turn OFF if it gets wet enough (above threshold + buffer)
  else if (currentMoisturePct >= (threshold + hysteresis)) {
    pumpIsOn = false;
  }

  // Set the Physical Relay
  if (pumpIsOn) {
    digitalWrite(relayPin, LOW);  
  } else {
    digitalWrite(relayPin, HIGH); 
  }

  // Print status to KB2040 Serial Monitor
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.print("Raw Sensor: "); Serial.print(rawValue);
    Serial.print(" | Moisture: "); Serial.print(currentMoisturePct); Serial.print("%");
    Serial.print(" | Threshold: "); Serial.print(threshold); Serial.print("%");
    Serial.print(" | Pump: "); Serial.println(pumpIsOn ? "ON" : "OFF");
  }
}

// ==================== I2C EVENT HANDLERS ============================

void receiveEvent(int howMany) {
  if (howMany < 1) return; 
  
  byte cmd = Wire.read(); 
  
  if (cmd == 1) trigger5SecondOverride(); 
  else if (cmd == 2) arduinoMistCommand = true; 
  else if (cmd == 3) arduinoMistCommand = false; 
  else if (cmd == 4 && howMany >= 3) {
    byte msb = Wire.read();
    byte lsb = Wire.read();
    threshold = (msb << 8) | lsb; // Update the percentage threshold
    Serial.print("I2C Command: New Threshold set to ");
    Serial.print(threshold); Serial.println("%");
  }
}

void requestEvent() {
  // Send the clean 0-100% value directly back to the Arduino
  byte msb = (currentMoisturePct >> 8) & 0xFF;
  byte lsb = currentMoisturePct & 0xFF;
  Wire.write(msb);
  Wire.write(lsb);
}