int sensorPin = A0;   // KB2040 analog pin
int relayPin = 7;     // GP7

int dryValue = 1000;  // RP2040 analog range is bigger!

void setup() {
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);
  Serial.begin(115200);
}

void loop() {

  int moisture = analogRead(sensorPin);
  Serial.println(moisture);

  if(moisture > dryValue){
    digitalWrite(relayPin, LOW);   // pump ON
  }
  else{
    digitalWrite(relayPin, HIGH);  // pump OFF
  }

  delay(2000);
}
