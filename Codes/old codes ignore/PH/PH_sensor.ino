// set to 9600 baud on serial monitor for correct output
// V+ 5V pin, G GND pin, Po A0 pin
const int phPin = A0;          // The analog pin connected to Po
float calibrationOffset = -3.71; // Adjust this value to calibrate the sensor

// Variables for averaging the readings
const int numReadings = 10;
int readings[numReadings];      
int readIndex = 0;              
long total = 0;                  
int average = 0;                

void setup() {
  Serial.begin(9600);
  
  // Initialize the array for averaging
  for (int i = 0; i < numReadings; i++) {
    readings[i] = 0;
  }
  
  Serial.println("pH Sensor Initialized.");
}

void loop() {
  // Subtract the last reading and add the new one for a rolling average
  total = total - readings[readIndex];
  readings[readIndex] = analogRead(phPin);
  total = total + readings[readIndex];
  readIndex = readIndex + 1;

  if (readIndex >= numReadings) {
    readIndex = 0;
  }

  average = total / numReadings;

  // Convert the 10-bit analog reading (0-1023) to a voltage (0-5V)
  float voltage = average * (5.0 / 1023.0);

  // Typical conversion formula for these modules: pH = 3.5 * voltage + offset
  float phValue = 3.5 * voltage + calibrationOffset;

  Serial.print("Analog: ");
  Serial.print(average);
  Serial.print(" | Voltage: ");
  Serial.print(voltage, 3);
  Serial.print(" V | pH: ");
  Serial.println(phValue, 2);

  delay(500); 
}
