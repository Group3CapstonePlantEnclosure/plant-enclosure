#include <Wire.h>
#include "Adafruit_VEML7700.h"
#include <DHT.h>

// ---- DHT11 ----
#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ---- VEML7700 ----
Adafruit_VEML7700 veml = Adafruit_VEML7700();

void setup() {
Serial.begin(9600);
delay(200);

// Start sensors
dht.begin();

if (!veml.begin()) {
Serial.println("Error: VEML7700 not found (check wiring / I2C address)");
while (1) { delay(10); }
}

// Optional tuning (good defaults)
// veml.setGain(VEML7700_GAIN_1_4);
// veml.setIntegrationTime(VEML7700_IT_200MS);

Serial.println("DHT11 + VEML7700 ready!");
}

void loop() {
// --- Read DHT11 ---
float humidity = dht.readHumidity();
float tempC = dht.readTemperature();
float tempF = dht.readTemperature(true);

bool dht_ok = !(isnan(humidity) || isnan(tempC) || isnan(tempF));

// --- Read VEML7700 ---
float lux = veml.readLux();

// --- Print results ---
Serial.println("---- Sensor Readings ----");

if (dht_ok) {
Serial.print("Humidity: ");
Serial.print(humidity);
Serial.println(" %");

Serial.print("Temp: ");
Serial.print(tempC);
Serial.print(" C / ");
Serial.print(tempF);
Serial.println(" F");
} else {
Serial.println("DHT11: read failed (check S pin, power, or sensor type)");
}

Serial.print("Light: ");
Serial.print(lux);
Serial.println(" lux");

Serial.println("-------------------------\n");

delay(2000); // DHT11 needs ~2 seconds between reads
}
