/**
 * Water Pump System for Adafruit KB2040 (I2C Peripheral Edition)
 * Logic: Pump ON if > threshold + hysteresis, Pump OFF if < threshold - hysteresis
 * Button/I2C Command: Simulates 1500 value for 5 seconds
 */

#include <Wire.h>

#define I2C_ADDR 0x08 // KB2040 I2C Address

int sensorPin = A0;
int relayPin = 7;
int mistPin = 8;
int buttonPin = 9;

// Threshold Logic
int threshold = 950; 
int hysteresis = 15;  // Buffer: ON at 1051+, OFF at 949-
bool pumpIsOn = false;

// Override variables
bool simulatedOverrideActive = false;
unsigned long simulatedOverrideStart = 0;
const unsigned long simulatedOverrideDuration = 5000; 

// Peripheral states
bool arduinoMistCommand = false;
bool buttonHandled = false;

// Global tracker for moisture to send over I2C
int currentMoistureToReport = 0;

void setup() {
  // Relay Setup (Active-Low: HIGH is OFF)
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); 

  pinMode(mistPin, OUTPUT);
  digitalWrite(mistPin, LOW);

  // Button with Internal Pullup
  pinMode(buttonPin, INPUT_PULLUP);

  Serial.begin(115200);

  // Initialize I2C via Qwiic (Peripheral/Slave Mode)
  Wire.begin(I2C_ADDR);
  Wire.onReceive(receiveEvent); // Triggered when Arduino sends a command
  Wire.onRequest(requestEvent); // Triggered when Arduino asks for moisture data
  
  Serial.println("KB2040 I2C Water Pump Controller Online");
}

void loop() {
  checkSerialMonitor();
  checkButton();
  
  // Constantly update what value we will send to the Arduino if asked
  currentMoistureToReport = simulatedOverrideActive ? 1500 : analogRead(sensorPin);
  
  updatePumpState();
  updateMistState();
  // sendMoistureData() is removed because I2C onRequest handles it automatically!
}

// Function to trigger the 5-second "dry" simulation
void trigger5SecondOverride() {
  simulatedOverrideActive = true;
  simulatedOverrideStart = millis();
  Serial.println("--- Manual Override: Forcing Pump ON (Simulating 1500) ---");
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

  // Reset timer if the physical pin state changes
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  // Only update the "true" state if it's been stable for 50ms
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
  int currentReading = analogRead(sensorPin);
  int actualSensorValue = currentReading; 

  // Handle the Button/Serial/I2C Override
  if (simulatedOverrideActive) {
    if (millis() - simulatedOverrideStart < simulatedOverrideDuration) {
      currentReading = 1500; // Force to high value to trigger ON
    } else {
      simulatedOverrideActive = false;
      Serial.println("--- Override Complete: Returning to Sensor Control ---");
    }
  }

  // --- HYSTERESIS LOGIC (Pump ON when High/Dry) ---
  if (currentReading > (threshold + hysteresis)) {
    pumpIsOn = true;
  } 
  else if (currentReading < (threshold - hysteresis)) {
    pumpIsOn = false;
  }

  // Set the Physical Relay (active-low relay)
  if (pumpIsOn) {
    digitalWrite(relayPin, LOW);  // Relay coil energized
  } else {
    digitalWrite(relayPin, HIGH); // Relay coil idle
  }

  // Status Monitor (Limited to once per second)
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.print("Raw Sensor: "); Serial.print(actualSensorValue);
    Serial.print(" | Logic Value: "); Serial.print(currentReading);
    Serial.print(" | Pump: "); Serial.println(pumpIsOn ? "ON" : "OFF");
  }
}

// ====================================================================
// ==================== I2C EVENT HANDLERS ============================
// ====================================================================

// Triggers when the Arduino Master SENDS data
void receiveEvent(int howMany) {
  if (howMany < 1) return; // Ignore empty transmissions
  
  byte cmd = Wire.read(); // Read the first byte (The Command ID)
  
  if (cmd == 1) {
    trigger5SecondOverride(); // PUMP ON COMMAND
  } 
  else if (cmd == 2) {
    arduinoMistCommand = true; // MIST ON
    Serial.println("I2C Command: Mist ON");
  } 
  else if (cmd == 3) {
    arduinoMistCommand = false; // MIST OFF
    Serial.println("I2C Command: Mist OFF");
  }
  else if (cmd == 4 && howMany >= 3) {
    // Threshold Update: expects Command(4) + MSB + LSB
    byte msb = Wire.read();
    byte lsb = Wire.read();
    threshold = (msb << 8) | lsb;
    Serial.print("I2C Command: New System Threshold set to ");
    Serial.println(threshold);
  }
}

// Triggers when the Arduino Master REQUESTS data
void requestEvent() {
  // Break the current reading into 2 bytes and send it back
  byte msb = (currentMoistureToReport >> 8) & 0xFF;
  byte lsb = currentMoistureToReport & 0xFF;
  
  Wire.write(msb);
  Wire.write(lsb);
}