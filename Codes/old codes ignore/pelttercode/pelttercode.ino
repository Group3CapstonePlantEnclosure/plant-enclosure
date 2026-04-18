#include <BTS7960.h>
#include <elapsedMillis.h>

// --- Pin Definitions ---
const uint8_t R_EN = 8;
const uint8_t L_EN = 9;
const uint8_t RPWM = 10;
const uint8_t LPWM = 11;

// --- New Fan Pin Definitions ---
const uint8_t TOP_FAN_PIN = 5;    // PWM pin for large top fan
const uint8_t BOTTOM_FAN_PIN = 6; // PWM pin for small bottom fan
// --- Initialize the Motor Driver Library ---
BTS7960 motorController(L_EN, R_EN, LPWM, RPWM);

// --- Slow PWM Settings ---
elapsedMillis slowPwmTimer;
const unsigned long PWM_PERIOD = 2000; // 2-second cycle
const float DUTY_CYCLE = 0.83;
// 83% duty cycle to limit to ~5A
const unsigned long ON_TIME = PWM_PERIOD * DUTY_CYCLE;
// --- Modes ---
enum Mode { COOLING, HEATING, OFF };
Mode currentMode = OFF; 

void setup() {
  Serial.begin(115200);

  motorController.Enable();
motorController.Stop();

  // Configure Fan Pins
  pinMode(TOP_FAN_PIN, OUTPUT);
  pinMode(BOTTOM_FAN_PIN, OUTPUT);
  // Ensure fans are OFF initially
  analogWrite(TOP_FAN_PIN, 0);
  analogWrite(BOTTOM_FAN_PIN, 0);
  
  Serial.println("--- Peltier Serial Controller ---");
  Serial.println("Send 'H' for Heating");
  Serial.println("Send 'C' for Cooling");
  Serial.println("Send 'O' to turn OFF");
  Serial.println("---------------------------------");
}

void loop() {
  // 1. Handle Serial Commands
  if (Serial.available() > 0) {
    char command = Serial.read();
if (command == 'H' || command == 'h') {
      if (currentMode == COOLING) {
        applySafetyPause();
// Prevent thermal shock
      } else if (currentMode == OFF) {
        applyStartupDelay();
// Prevent power supply voltage drops
      }
      
      currentMode = HEATING;
Serial.println(">>> Mode set to: HEATING");
      slowPwmTimer = 0; 

      // --- Add Fan Control for Heating ---
      analogWrite(TOP_FAN_PIN, 255);    // Large top fan FULL BLAST
      analogWrite(BOTTOM_FAN_PIN, 64);   // Small bottom fan SLOW
      
    } else if (command == 'C' || command == 'c') {
      if (currentMode == HEATING) {
        applySafetyPause();
// Prevent thermal shock
      } else if (currentMode == OFF) {
        applyStartupDelay();
// Prevent power supply voltage drops
      }
      
      currentMode = COOLING;
Serial.println(">>> Mode set to: COOLING");
      slowPwmTimer = 0; 

      // --- Add Fan Control for Cooling (Identical to Heating) ---
      analogWrite(TOP_FAN_PIN, 255);    // Large top fan FULL BLAST
      analogWrite(BOTTOM_FAN_PIN, 64);   // Small bottom fan SLOW
      
    } else if (command == 'O' || command == 'o') {
      currentMode = OFF;
Serial.println(">>> Mode set to: OFF");

      // --- Turn OFF both fans ---
      analogWrite(TOP_FAN_PIN, 0);
      analogWrite(BOTTOM_FAN_PIN, 0);
    }
  }

  // 2. Slow PWM Logic
  if (currentMode == OFF) {
    motorController.Stop();
return; // Skip the rest of the loop
  }

  if (slowPwmTimer >= PWM_PERIOD) {
    slowPwmTimer = 0;
}

  if (slowPwmTimer < ON_TIME) {
    // "ON" phase: Send full power (255)
    if (currentMode == HEATING) {
      motorController.TurnRight(255);
} else if (currentMode == COOLING) {
      motorController.TurnLeft(255);
}
  } else {
    // "OFF" phase
    motorController.Stop();
}
}

// --- Safety & Delay Functions ---

void applySafetyPause() {
  Serial.println("! THERMAL SHOCK PREVENTION !");
Serial.println("Waiting 5 seconds for temperatures to neutralize...");
  motorController.Stop();
  // We can also stop the fans here if desired, but 
  // keeping them on during neutralize might be better. 
  // Let's keep them as is for now during pause.
  delay(5000); 
  Serial.println("Resuming...");
}

void applyStartupDelay() {
  Serial.println("Applying startup delay (5 seconds) to stabilize power...");
  motorController.Stop();
  // Ensure fans are off during this delay as well
  analogWrite(TOP_FAN_PIN, 0);
  analogWrite(BOTTOM_FAN_PIN, 0);
  delay(5000);
  Serial.println("Starting...");
}