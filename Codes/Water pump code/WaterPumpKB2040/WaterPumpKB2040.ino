int sensorPin = A0;   // KB2040 analog pin
int relayPin = 7;     // GP7 (Pump Relay)
int mistPin = 8;      // GP8 (Mist Generator MOSFET Gate)

int dryValue = 1000;  // Default humidity/moisture threshold

// NEW: Hysteresis variables to prevent relay flickering
int hysteresis = 150; // How far the moisture must drop below dryValue before turning off
bool localWatering = false; // Remembers if the sensor triggered the pump

// Timer variables to replace delay()
unsigned long previousMoistureMillis = 0;
const long moistureInterval = 2000; 

// Override variables
unsigned long overrideMillis = 0;
bool overrideActive = false;

// Arduino command states
bool arduinoPumpCommand = false;
bool arduinoMistCommand = false; // NEW: Keeps track of the mist generator

void setup() {
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); // Ensure pump is OFF on boot (Relay is active LOW)
  
  pinMode(mistPin, OUTPUT);
  digitalWrite(mistPin, LOW);   // Ensure mist is OFF on boot (MOSFET is active HIGH)
  
  Serial.begin(115200);  // USB Serial for your Serial Monitor
  Serial1.begin(9600);   // Hardware TX/RX for Arduino Uno R4 communication
}

void loop() {
  checkSerialMonitor();   // Look for the "on" override
  checkArduinoCommands(); // Look for signals from the Uno R4
  updatePumpState();      // Turn the pump on or off based on our rules
  updateMistState();      // NEW: Turn the mist generator on or off
  sendMoistureData();     // Read A0 and send to the Uno R4
}

void checkSerialMonitor() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim(); // Clean up any hidden spaces or newlines
    
    // If you type "on" in the serial monitor, activate the 1-second override
    if (input.equalsIgnoreCase("on")) {
      overrideActive = true;
      overrideMillis = millis(); // Record the exact time the override started
      Serial.println("Manual override: Pump ON for 1 second");
    }
  }
}

void checkArduinoCommands() {
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim();
    
    if (command == "PUMP_ON") {
      arduinoPumpCommand = true;
      Serial.println("Arduino requested: Pump ON");
    } 
    else if (command == "PUMP_OFF") {
      arduinoPumpCommand = false;
      Serial.println("Arduino requested: Pump OFF");
    }
    // NEW: MIST COMMANDS
    else if (command == "MIST_ON") {
      arduinoMistCommand = true;
      Serial.println("Arduino requested: Mist ON");
    }
    else if (command == "MIST_OFF") {
      arduinoMistCommand = false;
      Serial.println("Arduino requested: Mist OFF");
    }
    else if (command.startsWith("HUM:")) {
      String valueStr = command.substring(4);
      dryValue = valueStr.toInt();
      Serial.print("Arduino set new dryValue threshold: ");
      Serial.println(dryValue);
    }
  }
}

void updateMistState() {
  // Check if the Arduino Uno R4 has requested the mist to be on
  if (arduinoMistCommand == true) {
    digitalWrite(mistPin, HIGH); // Send voltage to the MOSFET Gate to turn it ON
  } else {
    digitalWrite(mistPin, LOW);  // Remove voltage to turn it OFF
  }
}

void updatePumpState() {
  // 1. Normal operation: Check local sensor vs threshold with HYSTERESIS
  int currentMoisture = analogRead(sensorPin);
  
  // If moisture is very dry, set our watering flag to TRUE
  if (currentMoisture > dryValue) {
    localWatering = true;
  } 
  // If moisture is fully watered (below our gap), set watering flag to FALSE
  else if (currentMoisture < (dryValue - hysteresis)) {
    localWatering = false;
  }

  // 2. Handle the manual override timer
  bool overrideWantsPumpOn = false; // Create a temporary "vote" for the override
  if (overrideActive) {
    if (millis() - overrideMillis >= 1000) { // Change 1000 here to make the override last longer!
      overrideActive = false; // Time's up! Turn off override.
      Serial.println("Manual override finished.");
    } else {
      overrideWantsPumpOn = true; // We are still inside the override time window
    }
  }

  // 3. Final Decision: Combine all our rules!
  // The pump turns ON if the Override wants it, OR the Arduino wants it, OR the Sensor wants it.
  if (overrideWantsPumpOn == true || arduinoPumpCommand == true || localWatering == true) {
    digitalWrite(relayPin, LOW);  // Pump ON
  } else {
    digitalWrite(relayPin, HIGH); // Pump OFF
  }
}

void sendMoistureData() {
  unsigned long currentMillis = millis();
  
  // This acts like your delay(2000), but doesn't block the rest of the code!
  if (currentMillis - previousMoistureMillis >= moistureInterval) {
    previousMoistureMillis = currentMillis;
    
    int moisture = analogRead(sensorPin);
    
    // Send to Arduino Uno R4 via TX pin
    Serial1.print("MOISTURE:");
    Serial1.println(moisture);
    
    // Also print to Serial Monitor so you can watch it
    Serial.print("Current Moisture sent to Arduino: ");
    Serial.println(moisture);
  }
}