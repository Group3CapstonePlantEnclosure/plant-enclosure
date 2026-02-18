/*
  Device: ESP32
  Role: BLE Central (Client)
*/

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// UUIDs (Must match the R4 code)
static BLEUUID serviceUUID("19B10000-E8F2-537E-4F6C-D104768A1214");
static BLEUUID sensorCharUUID("19B10001-E8F2-537E-4F6C-D104768A1214");
static BLEUUID messageCharUUID("19B10002-E8F2-537E-4F6C-D104768A1214");

BLEClient* pClient;
BLERemoteCharacteristic* pSensorCharacteristic;
BLERemoteCharacteristic* pMessageCharacteristic;
bool connected = false;

// Callback function: triggers when R4 sends new sensor data
// Callback function: triggers when R4 sends new sensor data
static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
    
  // Create a temporary string using the exact length of the received data
  std::string value((char*)pData, length);
  
  Serial.print("New Data from R4: ");
  Serial.println(value.c_str());
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting BLE Client...");

  BLEDevice::init("");
  
  // Start scanning for the R4
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

void loop() {
  // 1. If not connected, try to connect
  if (!connected) {
    if (connectToR4()) {
      Serial.println("Connected! Type a message in the input box above.");
    }
  } 
  
  // 2. If connected, check for user input from Serial Monitor
  if (connected) {
    if (Serial.available()) {
      // Read the text you typed until you hit Enter
      String message = Serial.readStringUntil('\n');
      message.trim(); // Removes extra spaces or newlines

      if (message.length() > 0) {
        Serial.print("Sending to R4: ");
        Serial.println(message);

        // Send the message over BLE
        if (pMessageCharacteristic->canWrite()) {
          pMessageCharacteristic->writeValue(message.c_str(), message.length());
        }
      }
    }
  }
  
  delay(10); // Small delay for stability
}

bool connectToR4() {
  Serial.println("Scanning for Uno R4...");
  BLEScan* pBLEScan = BLEDevice::getScan();
  
  // FIX: Use a pointer (*) for foundDevices
  BLEScanResults *foundDevices = pBLEScan->start(3, false);
  
  BLEAdvertisedDevice* myDevice = nullptr;

  // FIX: Use '->' to access getCount()
  Serial.print("Devices found: ");
  Serial.println(foundDevices->getCount());

  // Look through found devices
  for (int i = 0; i < foundDevices->getCount(); i++) {
    // FIX: Use '->' to access getDevice()
    BLEAdvertisedDevice device = foundDevices->getDevice(i);
    
    if (device.haveServiceUUID() && device.isAdvertisingService(serviceUUID)) {
      Serial.println("Found R4!");
      myDevice = new BLEAdvertisedDevice(device);
      break;
    }
  }

  // Clear the scan results to free memory
  pBLEScan->clearResults();

  if (myDevice == nullptr) {
    Serial.println("R4 not found. Retrying...");
    return false;
  }

  // Connect
  pClient = BLEDevice::createClient();
  // Note: Some ESP32 versions require passing the address directly if 'myDevice' fails
  if (pClient->connect(myDevice)) {
    Serial.println("Connected to Server");
  } else {
    Serial.println("Failed to connect");
    return false;
  }

  // Get Service
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find our service UUID");
    return false;
  }

  // Get Sensor Characteristic (Read/Notify)
  pSensorCharacteristic = pRemoteService->getCharacteristic(sensorCharUUID);
  if (pSensorCharacteristic == nullptr) {
    Serial.println("Failed to find sensor characteristic");
    return false;
  }
  
  if (pSensorCharacteristic->canNotify()) {
    pSensorCharacteristic->registerForNotify(notifyCallback);
  }

  // Get Message Characteristic (Write)
  pMessageCharacteristic = pRemoteService->getCharacteristic(messageCharUUID);
  if (pMessageCharacteristic == nullptr) {
    Serial.println("Failed to find message characteristic");
    return false;
  }
  
  connected = true;
  return true;
}