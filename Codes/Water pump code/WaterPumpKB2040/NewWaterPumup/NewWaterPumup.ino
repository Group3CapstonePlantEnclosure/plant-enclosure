/**
 * Water Pump System for Adafruit KB2040
 * Logic: Pump ON if > 1050, Pump OFF if < 950
 * Button: Simulates 1500 value for 5 seconds
 */

int sensorPin = A0;
int relayPin = 7;
int mistPin = 8;
int buttonPin = 9;

// Threshold Logic
int threshold = 1000; 
int hysteresis = 50;  // Buffer: ON at 1051+, OFF at 949-
bool pumpIsOn = false;

// Timing for Moisture Updates
unsigned long previousMoistureMillis = 0;
const unsigned long moistureInterval = 2000;

// Override variables
bool simulatedOverrideActive = false;
unsigned long simulatedOverrideStart = 0;
const unsigned long simulatedOverrideDuration = 5000; 

// Peripheral states
bool arduinoMistCommand = false;
bool buttonHandled = false;

void setup() {
  // Relay Setup (Active-Low: HIGH is OFF)
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); 

  pinMode(mistPin, OUTPUT);
  digitalWrite(mistPin, LOW);

  // Button with Internal Pullup
  pinMode(buttonPin, INPUT_PULLUP);

  Serial.begin(115200);
  Serial1.begin(9600);
}

void loop() {
  checkSerialMonitor();
  checkArduinoCommands();
  checkButton();
  updatePumpState();
  updateMistState();
  sendMoistureData();
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

void checkArduinoCommands() {
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim();
    if (command == "PUMP_ON") trigger5SecondOverride();
    else if (command == "MIST_ON") arduinoMistCommand = true;
    else if (command == "MIST_OFF") arduinoMistCommand = false;
    else if (command.startsWith("HUM:")) {
      threshold = command.substring(4).toInt();
      Serial.print("New System Threshold: ");
      Serial.println(threshold);
    }
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

  // Handle the Button/Serial Override
  if (simulatedOverrideActive) {
    if (millis() - simulatedOverrideStart < simulatedOverrideDuration) {
      currentReading = 1500; // Force to high value to trigger ON
    } else {
      simulatedOverrideActive = false;
      Serial.println("--- Override Complete: Returning to Sensor Control ---");
    }
  }

  // --- HYSTERESIS LOGIC (Pump ON when High/Dry) ---
  // Turn ON if value goes above upper bound
  if (currentReading > (threshold + hysteresis)) {
    pumpIsOn = true;
  } 
  // Turn OFF if value drops below lower bound
  else if (currentReading < (threshold - hysteresis)) {
    pumpIsOn = false;
  }

  // Set the Physical Relay (assuming active-low relay)
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

void sendMoistureData() {
  if (millis() - previousMoistureMillis >= moistureInterval) {
    previousMoistureMillis = millis();
    
    // Send simulated value if override is active
    int val = simulatedOverrideActive ? 1500 : analogRead(sensorPin);
    
    Serial1.print("MOISTURE:");
    Serial1.println(val);
  }
}