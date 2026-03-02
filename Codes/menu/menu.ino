#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <time.h>

// --- MULTITASKING GLOBALS ---
TaskHandle_t CloudTaskHandle;
volatile bool triggerCloudPush = false;

// --- SERIAL COMMUNICATION CONFIG ---
#define RXD2 16
#define TXD2 17

// Global "Live" Data
float liveTemp = 0.0;
float liveHum = 0.0;
float liveLux = 0.0;
unsigned long lastSerialRecv = 0;

// --- WIFI CREDENTIALS ---
struct WiFiCreds { const char* ssid; const char* pass; };
WiFiCreds myNetworks[] = {
  { "MASCU-Fi", "milliondollarbackyard@1192" },
  { "shanel", "jamescharles" }
};

// --- PIN DEFINITIONS ---
#define I2C_SDA 21
#define I2C_SCL 22
#define BOTTOM_ADDR 0x3C
#define TOP_ADDR 0x3D
#define ENC_CLK 32
#define ENC_DT 33
#define ENC_SW 25
#define I2CBusClock 250000

// --- DISPLAY OBJECTS ---
U8G2_SH1106_128X64_NONAME_F_HW_I2C topDisplay(U8G2_R0, 22, 21, U8X8_PIN_NONE);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C bottomDisplay(U8G2_R0, 22, 21, U8X8_PIN_NONE);

// --- SETTINGS VARIABLES ---
float tempLow = 68.0, tempHigh = 77.0, humLow = 40.0, humHigh = 80.0;
float soilLow = 30.0, soilHigh = 70.0;
bool timerEnabled = false;
int timeOnHour = 8, timeOnMinute = 0, timeOffHour = 20, timeOffMinute = 0;
int globalBrightness = 10;
long luxThreshold = 30000;
int timeZoneOffset = -5; // Default EST

// System Clock
int currentHour = 12, currentMinute = 0;
unsigned long lastMinuteTick = 0;

// Edit State Helpers
float *pEditVal1 = nullptr, *pEditVal2 = nullptr, editCurrent = 0;
int editStep = 0;
float currentMinLimit = 0, currentMaxLimit = 100;
String editUnit = "", currentHeaderName = "-- MAIN MENU --";
int tempOnH, tempOnM, tempOffH, tempOffM;
int lastMenuIdx = -1;
float lastValDisp = -999;
int lastStepDisp = -1;

// Notifications & Updates
bool externalUpdateReceived = false;
unsigned long bottomMsgTimeout = 0;
bool showingTempMsg = false;
unsigned long lastEncoderMoveTime = 0;
unsigned long lastCloudCheck = 0;
String webLogBuffer = "Device Booted. Serial Mode.\n";
const char* firebaseURL = "https://plant-enclosure-default-rtdb.firebaseio.com/settings.json";

// Menu Logic
struct MenuItem {
  const char* name;
  void (*action)();
  MenuItem* children;
  uint8_t childCount;
};

enum UIState { STATE_MENU, STATE_SUBMENU, STATE_EDIT_DUAL, STATE_EDIT_TIME, 
               STATE_EDIT_LUX, STATE_EDIT_BRIGHTNESS, STATE_SENSOR_TEST, STATE_WIFI_SELECT };
UIState uiState = STATE_MENU;

MenuItem *currentMenu = nullptr, *menuStack[6];
uint8_t currentMenuSize = 0, menuSizeStack[6], stackDepth = 0;
int selectedIndex = 0, menuScrollOffset = 0, indexStack[6];
const int VISIBLE_ITEMS = 4;

volatile int encoderCount = 0;
int lastEncoderCount = 0;
static const int8_t enc_states[] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
static uint8_t old_AB = 0;

// --- SERIAL PARSER & LIVE CLOUD UPLOAD ---
void pushLiveDataToCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload Failed: WiFi not connected.");
    return;
  }
  
  HTTPClient http; 
  http.begin(firebaseURL); 
  http.addHeader("Content-Type", "application/json");
  
  StaticJsonDocument<200> doc; 
  doc["liveTemp"] = liveTemp; 
  doc["liveHum"] = liveHum; 
  doc["liveLux"] = liveLux; 
  
  String jsonOutput; 
  serializeJson(doc, jsonOutput); 
  
  int httpCode = http.sendRequest("PATCH", jsonOutput);
  if (httpCode > 0) {
    Serial.println("Firebase Success! HTTP Code: " + String(httpCode));
  } else {
    Serial.println("Firebase Failed! Error: " + http.errorToString(httpCode));
    sysLog("Failed to push live data: " + http.errorToString(httpCode));
  }
  http.end(); 
}

void checkSerialSensors() {
  bool newDataReceived = false;

  while (Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if (data.length() > 0) {
      int tIndex = data.indexOf("T:");
      int hIndex = data.indexOf("H:");
      int lIndex = data.indexOf("L:");
      
      if (tIndex != -1 && hIndex != -1 && lIndex != -1) {
        int comma1 = data.indexOf(',', tIndex);
        int comma2 = data.indexOf(',', hIndex);
        liveTemp = data.substring(tIndex + 2, comma1).toFloat();
        liveHum = data.substring(hIndex + 2, comma2).toFloat();
        liveLux = data.substring(lIndex + 2).toFloat();
        lastSerialRecv = millis();
        newDataReceived = true; 
      }
    }
  }

  if (newDataReceived) {
    externalUpdateReceived = true; 
  }
}

// --- FORWARD DECLARATIONS ---
void updateBottomMenu(String line1, String line2 = ""); 
void startEditTemp(); void startEditHum(); void startEditSoil(); void toggleTimer(); 
void startEditLux(); void resetGlobal(); void showPH(); void startSensorTest(); 
void startSetClock(); void startEditBrightness(); void goBack(); void startWiFiSetup(); 
void showWiFiIP(); void resetWiFi(); void pushToCloud(); void syncWithCloudSilent(); 
void triggerESPReset(); void startWiFiSelect(); void syncWithCloud();
void setTimeZone(int offset, String name);
void pushLiveDataToCloud(); 

// --- TIME ZONE SETTERS ---
void setEST() { setTimeZone(-5, "EST (UTC-5)"); }
void setCST() { setTimeZone(-6, "CST (UTC-6)"); }
void setMST() { setTimeZone(-7, "MST (UTC-7)"); }
void setPST() { setTimeZone(-8, "PST (UTC-8)"); }
void setUTC() { setTimeZone(0, "UTC"); }

void setTimeZone(int offset, String name) {
  timeZoneOffset = offset;
  configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
  updateBottomMenu("Time Zone", name);
  triggerCloudPush = true; // Tell Core 0 to save it
  delay(1500);
  goBack();
}

// --- STANDARD FUNCTIONS ---
void IRAM_ATTR readEncoder() {
  old_AB <<= 2; old_AB |= ((digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT));
  old_AB &= 0x0f; if (enc_states[old_AB]) encoderCount += enc_states[old_AB];
}

void sysLog(String msg) {
  Serial.println(msg);
  String timeStr = "[" + String(currentHour) + ":" + (currentMinute < 10 ? "0" : "") + String(currentMinute) + "] ";
  webLogBuffer += timeStr + msg + "\n";
  if (webLogBuffer.length() > 1500) webLogBuffer = webLogBuffer.substring(webLogBuffer.length() - 1000);
}

// --- MENUS ---
MenuItem tempItems[] = { { "Set Range", startEditTemp, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem humItems[] = { { "Set Range", startEditHum, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem soilItems[] = { { "Set Range", startEditSoil, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem lightItems[] = { { "Timer: OFF", toggleTimer, nullptr, 0 }, { "Set LUX Limit", startEditLux, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };

MenuItem wifiItems[] = { 
  { "Select Network", startWiFiSelect, nullptr, 0 }, 
  { "Setup (AP Mode)", startWiFiSetup, nullptr, 0 }, 
  { "Show IP", showWiFiIP, nullptr, 0 }, 
  { "Reset WiFi", resetWiFi, nullptr, 0 }, 
  { "Back", nullptr, nullptr, 0 } 
};

MenuItem timeZoneItems[] = {
  { "EST (Eastern)", setEST, nullptr, 0 },
  { "CST (Central)", setCST, nullptr, 0 },
  { "MST (Mountain)", setMST, nullptr, 0 },
  { "PST (Pacific)", setPST, nullptr, 0 },
  { "UTC", setUTC, nullptr, 0 },
  { "Back", nullptr, nullptr, 0 }
};

MenuItem settingsItems[] = {
  { "WiFi", nullptr, wifiItems, 5 },
  { "Time Zone", nullptr, timeZoneItems, 6 }, 
  { "Brightness", startEditBrightness, nullptr, 0 },
  { "Sensor Test", startSensorTest, nullptr, 0 },
  { "Global Reset", resetGlobal, nullptr, 0 },
  { "ESP32 Reset", triggerESPReset, nullptr, 0 },
  { "Back", nullptr, nullptr, 0 }
};

MenuItem mainMenu[] = {
  { "Temperature", nullptr, tempItems, 2 },
  { "Humidity", nullptr, humItems, 2 },
  { "Soil Moisture", nullptr, soilItems, 2 },
  { "Light Control", nullptr, lightItems, 3 },
  { "pH Level", showPH, nullptr, 0 },
  { "Settings", nullptr, settingsItems, 7 }
};

// --- UTILS ---
void applyBrightness(int level) {
  int contrast = map(level, 1, 10, 1, 255);
  topDisplay.setContrast(contrast); bottomDisplay.setContrast(contrast);
}

String getLuxPlantType(long lux) {
  if (lux < 2500) return "Low: Pothos/Snake";
  if (lux < 10000) return "Med: Ferns/Peace";
  if (lux < 20000) return "High: Monstera";
  if (lux < 50000) return "Direct: Herbs";
  return "Full Sun: Crops";
}

void updateClock() {
  if (millis() - lastMinuteTick >= 60000) {
    lastMinuteTick = millis();
    if (WiFi.status() == WL_CONNECTED) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) { currentHour = timeinfo.tm_hour; currentMinute = timeinfo.tm_min; return; }
    }
    currentMinute++;
    if (currentMinute > 59) { currentMinute = 0; currentHour++; if (currentHour > 23) currentHour = 0; }
  }
}

String getCountdownStr() {
  if (!timerEnabled) return "Timer: OFF"; // Shortened for fit
  
  int nowMins = (currentHour * 60) + currentMinute;
  int onMins = (timeOnHour * 60) + timeOnMinute;
  int offMins = (timeOffHour * 60) + timeOffMinute;
  
  bool isLightOn = false;
  
  if (onMins < offMins) {
    if (nowMins >= onMins && nowMins < offMins) isLightOn = true;
  } else if (onMins > offMins) {
    if (nowMins >= onMins || nowMins < offMins) isLightOn = true;
  }
  
  int diff = (isLightOn ? offMins : onMins) - nowMins;
  if (diff <= 0) diff += 1440; 
  
  int h = diff / 60;
  int m = diff % 60;
  
  // FIX: Shortened format to fit the 128px OLED screen
  String timeStr = String(h) + "h" + (m < 10 ? "0" : "") + String(m) + "m";
  
  return isLightOn ? "ON for " + timeStr : "OFF for " + timeStr;
}

int getCenterX(U8G2& display, String text) { return (display.getDisplayWidth() - display.getStrWidth(text.c_str())) / 2; }

void updateBottomMenu(String line1, String line2) {
  bottomDisplay.clearBuffer(); bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.setCursor(0, 10); bottomDisplay.print(line1);
  if (line2 != "") { bottomDisplay.setFont(u8g2_font_helvB12_tr); bottomDisplay.setCursor(0, 30); bottomDisplay.print(line2); }
  bottomDisplay.sendBuffer();
}

String getPreviewTime(int offset) {
  time_t now;
  time(&now); 
  if (now < 100000) return "No WiFi Time"; 
  time_t local = now + (offset * 3600);
  struct tm* ti = gmtime(&local); 
  int h = ti->tm_hour;
  int m = ti->tm_min;
  String ampm = (h < 12) ? " AM" : " PM";
  h = h % 12; if (h == 0) h = 12;
  String sM = (m < 10) ? "0" + String(m) : String(m);
  return String(h) + ":" + sM + ampm;
}

void showHoverContext(const char* itemName) {
  String name = String(itemName);
  String valLine = "";
  
  if (name == "Temperature") valLine = String(liveTemp, 1) + "\xb0 | L:" + String((int)tempLow) + "\xb0 H:" + String((int)tempHigh) + "\xb0";
  else if (name == "Humidity") valLine = String(liveHum, 0) + "% | L:" + String((int)humLow) + "% H:" + String((int)humHigh) + "%";
  else if (name == "Soil Moisture") valLine = "L:" + String((int)soilLow) + "% H:" + String((int)soilHigh) + "%";
  else if (name == "Light Control") valLine = String(liveLux, 0) + "lx | " + getCountdownStr();
  else if (name == "pH Level") valLine = "Current: 7.0";
  else if (name == "Settings") valLine = "System Setup";
  else if (name == "Connections") valLine = (WiFi.status() == WL_CONNECTED) ? "WiFi: ON" : "WiFi: OFF";
  else if (name == "Time Zone") valLine = "UTC " + String(timeZoneOffset);
  else if (name == "EST (Eastern)") valLine = getPreviewTime(-5);
  else if (name == "CST (Central)") valLine = getPreviewTime(-6);
  else if (name == "MST (Mountain)") valLine = getPreviewTime(-7);
  else if (name == "PST (Pacific)") valLine = getPreviewTime(-8);
  else if (name == "UTC") valLine = getPreviewTime(0);
  else if (String(itemName).startsWith("Timer")) valLine = timerEnabled ? "Status: ON" : "Status: OFF";
  else if (name == "Set Clock") {
    int h = currentHour % 12; if (h == 0) h = 12;
    String m = (currentMinute < 10) ? "0" + String(currentMinute) : String(currentMinute);
    valLine = "Time: " + String(h) + ":" + m + (currentHour < 12 ? " AM" : " PM");
  } 
  else if (name == "Brightness") valLine = "Level: " + String(globalBrightness) + "/10";
  else valLine = "Select to Edit";
  
  updateBottomMenu(name, valLine);
}

void drawWiFiStatus() {
  int x = 110; int y = 2;
  if (millis() - lastSerialRecv < 5000) topDisplay.drawStr(95, 8, "S");
  if (WiFi.status() != WL_CONNECTED) { topDisplay.drawLine(x, y, x + 8, y + 8); topDisplay.drawLine(x + 8, y, x, y + 8); } 
  else { long rssi = WiFi.RSSI(); topDisplay.drawBox(x, y + 6, 2, 2); if (rssi > -75) topDisplay.drawBox(x + 3, y + 3, 2, 5); if (rssi > -55) topDisplay.drawBox(x + 6, y, 2, 8); }
}

void drawTimeEdit(int h, int m, bool editingHour, String title) {
  topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.setCursor(getCenterX(topDisplay, title), 12); topDisplay.print(title);
  topDisplay.setFont(u8g2_font_logisoso24_tr); topDisplay.setCursor(15, 50);
  int dispH = h % 12; if (dispH == 0) dispH = 12;
  if (dispH < 10) topDisplay.print("0"); topDisplay.print(dispH);
  topDisplay.setCursor(55, 47); topDisplay.print(":");
  topDisplay.setCursor(70, 50); if (m < 10) topDisplay.print("0"); topDisplay.print(m);
  topDisplay.setFont(u8g2_font_helvB10_tr); topDisplay.setCursor(105, 50); topDisplay.print(h < 12 ? "AM" : "PM");
  topDisplay.setFont(u8g2_font_6x10_tf); topDisplay.drawStr(editingHour ? 30 : 80, 62, "^");
  topDisplay.sendBuffer();
  updateBottomMenu(title, String(dispH) + ":" + ((m < 10) ? "0" : "") + String(m) + (h < 12 ? " AM" : " PM"));
}

void drawLuxEdit(long currentLux) {
  String info = getLuxPlantType(currentLux);
  topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.setCursor(getCenterX(topDisplay, info), 20); topDisplay.print(info);
  topDisplay.drawFrame(10, 30, 108, 12);
  int w = map(currentLux, 0, 120000, 0, 106); if (w > 106) w = 106;
  topDisplay.drawBox(11, 31, w, 10);
  topDisplay.setFont(u8g2_font_6x10_tf); topDisplay.setCursor(10, 55);
  topDisplay.print(currentLux); topDisplay.print(" lux"); topDisplay.sendBuffer();
  updateBottomMenu("Light Threshold", String(currentLux) + " lux");
}

void drawBrightnessEdit(int level) {
  topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.drawStr(20, 15, "BRIGHTNESS"); topDisplay.drawFrame(10, 25, 108, 14);
  int w = map(level, 1, 10, 5, 106); topDisplay.drawBox(11, 26, w, 12);
  topDisplay.setFont(u8g2_font_6x10_tf); topDisplay.setCursor(60, 55);
  topDisplay.print(level); topDisplay.print("/10"); topDisplay.sendBuffer();
  updateBottomMenu("Adjust Level", String(level));
}

void drawCircularGauge(float valLower, float valUpper, float minLimit, float maxLimit, bool editingUpper) {
  topDisplay.clearBuffer();
  int cx = 64, cy = 32, r_outer = 30, r_inner = 22, r_mid = (r_outer + r_inner) / 2, r_brush = (r_outer - r_inner) / 2;
  float range = maxLimit - minLimit; if (range <= 0) range = 1;
  float angleStart = ((valLower - minLimit) / range) * 6.28 - 1.57;
  float angleEnd = ((valUpper - minLimit) / range) * 6.28 - 1.57;
  float a1 = min(angleStart, angleEnd), a2 = max(angleStart, angleEnd), step = 0.05;
  for (float a = a1; a <= a2; a += step) topDisplay.drawDisc(cx + r_mid * cos(a), cy + r_mid * sin(a), r_brush);
  topDisplay.drawDisc(cx + r_mid * cos(a2), cy + r_mid * sin(a2), r_brush);
  topDisplay.setFont(u8g2_font_4x6_tf);
  String label = editingUpper ? "UPPER" : "LOWER";
  topDisplay.setCursor(cx - (topDisplay.getStrWidth(label.c_str()) / 2), cy - 4); topDisplay.print(label);
  topDisplay.setFont(u8g2_font_6x10_tf);
  String valStr = String(editingUpper ? (int)valUpper : (int)valLower) + ((editUnit == "F") ? "\xb0" : "%");
  topDisplay.setCursor(cx - (topDisplay.getStrWidth(valStr.c_str()) / 2), cy + 6); topDisplay.print(valStr);
  topDisplay.sendBuffer();
}

void updateBottomEdit(String label, float currentVal, String refLabel, float refVal) {
  bottomDisplay.clearBuffer(); bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.setCursor(0, 10); bottomDisplay.print(label);
  String refText = refLabel + String((int)refVal) + ((editUnit == "F") ? "\xb0" : "%");
  bottomDisplay.setCursor(128 - bottomDisplay.getStrWidth(refText.c_str()), 10); bottomDisplay.print(refText);
  bottomDisplay.setFont(u8g2_font_helvB14_tr);
  String valText = String((int)currentVal) + ((editUnit == "F") ? "\xb0" : "%");
  bottomDisplay.setCursor((128 - bottomDisplay.getStrWidth(valText.c_str())) / 2, 31); bottomDisplay.print(valText);
  bottomDisplay.sendBuffer();
}

void drawSensorTest() {
  topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_helvB14_tr);
  topDisplay.drawStr(10, 30, "Sensor Live");
  topDisplay.setFont(u8g2_font_6x10_tf);
  
  if (millis() - lastSerialRecv < 5000) {
    topDisplay.drawStr(20, 50, "(Receiving)");
  } else {
    topDisplay.drawStr(20, 50, "(No Data)");
  }
  
  topDisplay.sendBuffer();
  bottomDisplay.clearBuffer(); bottomDisplay.setFont(u8g2_font_6x10_tf);
  String line1 = "T:" + String(liveTemp, 1) + "F  H:" + String(liveHum, 0) + "%";
  String line2 = "L:" + String(liveLux, 0);
  bottomDisplay.drawStr(0, 15, line1.c_str()); bottomDisplay.drawStr(0, 30, line2.c_str());
  bottomDisplay.sendBuffer();
}

// --- WIFI MANAGER & SETUP ---
void configModeCallback(WiFiManager* myWiFiManager) {
  topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_6x10_tf);
  topDisplay.drawStr(10, 20, "WIFI PORTAL OPEN");
  topDisplay.drawStr(10, 35, "SSID: Plant_Setup"); topDisplay.drawStr(10, 50, "PASS: plantadmin");
  topDisplay.sendBuffer();
  bottomDisplay.clearBuffer(); bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.drawStr(0, 10, "Click knob to exit");
  bottomDisplay.setFont(u8g2_font_helvB12_tr);
  bottomDisplay.drawStr(0, 30, WiFi.softAPIP().toString().c_str());
  bottomDisplay.sendBuffer();
}

void startWiFiSetup() {
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalBlocking(false); 
  
  if (wm.startConfigPortal("Plant_Setup", "plantadmin")) {
    unsigned long startStr = millis();
    while (millis() - startStr < 120000) { 
      wm.process(); 
      if (digitalRead(ENC_SW) == LOW) {
        delay(50); while(digitalRead(ENC_SW) == LOW); 
        wm.stopConfigPortal();
        updateBottomMenu("WiFi Setup", "Cancelled"); delay(1000); break;
      }
      if (WiFi.status() == WL_CONNECTED) {
        updateBottomMenu("Connected!", WiFi.localIP().toString());
        wm.stopConfigPortal(); delay(2000); break;
      }
    }
  } else { updateBottomMenu("WiFi Error", "Try Again"); delay(1000); }
  goBack();
}

void showWiFiIP() {
  String status = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Not Connected";
  updateBottomMenu("Current IP:", status); delay(3000); lastMenuIdx = -1;
}
void resetWiFi() { WiFiManager wm; wm.resetSettings(); updateBottomMenu("WiFi Reset", "Successful"); delay(2000); lastMenuIdx = -1; }

// --- SETTINGS EDIT FUNCTIONS ---
void startEditTemp() { uiState = STATE_EDIT_DUAL; editStep = 0; pEditVal1 = &tempLow; pEditVal2 = &tempHigh; currentMinLimit = 20.0; currentMaxLimit = 100.0; editUnit = "F"; editCurrent = *pEditVal1; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; lastValDisp = -999; }
void startEditHum() {  uiState = STATE_EDIT_DUAL; editStep = 0; pEditVal1 = &humLow; pEditVal2 = &humHigh; currentMinLimit = 0.0; currentMaxLimit = 100.0; editUnit = "%"; editCurrent = *pEditVal1; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; lastValDisp = -999; }
void startEditSoil() { uiState = STATE_EDIT_DUAL; editStep = 0; pEditVal1 = &soilLow; pEditVal2 = &soilHigh; currentMinLimit = 0.0; currentMaxLimit = 100.0; editUnit = "%"; editCurrent = *pEditVal1; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; lastValDisp = -999; }
void startEditSchedule() { uiState = STATE_EDIT_TIME; editStep = 0; tempOnH = timeOnHour; tempOnM = timeOnMinute; tempOffH = timeOffHour; tempOffM = timeOffMinute; editCurrent = tempOnH; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void toggleTimer() { if (!timerEnabled) { timerEnabled = true; startEditSchedule(); } else {timerEnabled = false; updateBottomMenu("Timer", "Disabled"); triggerCloudPush = true; delay(800); }}
void startSetClock() { uiState = STATE_EDIT_TIME; editStep = 10; editCurrent = currentHour; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void startEditLux() { uiState = STATE_EDIT_LUX; editCurrent = luxThreshold; encoderCount = (luxThreshold / 100) * 4; lastEncoderCount = encoderCount / 4; }
void startEditBrightness() { uiState = STATE_EDIT_BRIGHTNESS; editCurrent = globalBrightness; encoderCount = globalBrightness * 4; lastEncoderCount = globalBrightness; }
void resetGlobal() { tempLow = 68.0; tempHigh = 77.0; humLow = 50.0; humHigh = 60.0; soilLow = 40; soilHigh = 60; timeOnHour = 8; timeOnMinute = 0; timeOffHour = 20; timeOffMinute = 0; luxThreshold = 50000; timerEnabled = false; globalBrightness = 10; applyBrightness(globalBrightness); updateBottomMenu("Defaults", "Restored"); triggerCloudPush = true; delay(1500); lastMenuIdx = -1; }
void showPH() { updateBottomMenu("pH: 7.0", "Sensor OK"); delay(2000); lastMenuIdx = -1; }
void startSensorTest() { uiState = STATE_SENSOR_TEST; }

void startWiFiSelect() { uiState = STATE_WIFI_SELECT; currentMenuSize = (sizeof(myNetworks) / sizeof(myNetworks[0])) + 1; selectedIndex = 0; menuScrollOffset = 0; currentHeaderName = "KNOWN NETWORKS"; lastMenuIdx = -1; encoderCount = 0; lastEncoderCount = 0; }
void attemptConnection(const char* ssid, const char* pass) {
  updateBottomMenu("Connecting to:", String(ssid)); 
  WiFi.disconnect(); 
  WiFi.begin(ssid, pass);
  unsigned long start = millis(); bool success = false;
  
  while (millis() - start < 5000) { 
    if (WiFi.status() == WL_CONNECTED) { success = true; break; } 
    delay(100); 
  }
  
  if (success) { updateBottomMenu("Success!", WiFi.localIP().toString()); configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov"); syncWithCloud(); } 
  else { updateBottomMenu("Failed", "Check Password"); } delay(1500); goBack();
}

void triggerESPReset() { updateBottomMenu("REBOOTING", "Please wait..."); sysLog("Manual hardware reset."); delay(1000); ESP.restart(); }

void goBack() {
  if (stackDepth == 0) { 
    currentMenu = mainMenu; 
    currentMenuSize = 6; 
    selectedIndex = 0; uiState = STATE_MENU; currentHeaderName = "-- MAIN MENU --"; 
  } else { 
    stackDepth--; currentMenu = menuStack[stackDepth]; currentMenuSize = menuSizeStack[stackDepth]; selectedIndex = indexStack[stackDepth]; uiState = (stackDepth == 0) ? STATE_MENU : STATE_SUBMENU; 
    currentHeaderName = (stackDepth > 0) ? String(menuStack[stackDepth - 1][indexStack[stackDepth - 1]].name) : "-- MAIN MENU --"; currentHeaderName.toUpperCase(); 
  }
  menuScrollOffset = 0; lastMenuIdx = -1;
}
void enterSubMenu(MenuItem* item) {
  menuStack[stackDepth] = currentMenu; menuSizeStack[stackDepth] = currentMenuSize; indexStack[stackDepth] = selectedIndex;
  currentHeaderName = String(item->name); currentHeaderName.toUpperCase();
  stackDepth++; currentMenu = item->children; currentMenuSize = item->childCount; selectedIndex = 0; menuScrollOffset = 0; uiState = STATE_SUBMENU; lastMenuIdx = -1;
}

// ==========================================
// CLOUD SYNC & COMMAND PROCESSING
// ==========================================
void syncWithCloud() { if (WiFi.status() != WL_CONNECTED) return; updateBottomMenu("Syncing...", "Fetching Cloud"); sysLog("Cloud sync..."); syncWithCloudSilent(); }

void syncWithCloudSilent() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.begin(firebaseURL); int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString(); DynamicJsonDocument doc(2048); deserializeJson(doc, payload);
    bool changed = false;

    if (doc.containsKey("reboot_cmd") && doc["reboot_cmd"].as<bool>() == true) {
      sysLog("Cloud Restart Command Received! Rebooting...");
      updateBottomMenu("REMOTE REBOOT", "PLEASE WAIT");
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"reboot_cmd\":false}"); patchHttp.end();
      delay(1000); ESP.restart();
    }

    if (doc.containsKey("fetch_logs_cmd") && doc["fetch_logs_cmd"].as<bool>() == true) {
      sysLog("Web Dashboard requested log dump.");
      DynamicJsonDocument logDoc(2048);
      logDoc["fetch_logs_cmd"] = false;
      logDoc["system_logs"] = webLogBuffer;
      String logJson; serializeJson(logDoc, logJson);
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", logJson); patchHttp.end();
    }

    if (doc.containsKey("global_reset_cmd") && doc["global_reset_cmd"].as<bool>() == true) {
      sysLog("Web Command: Global Reset Initiated.");
      updateBottomMenu("GLOBAL RESET", "PLEASE WAIT");
      resetGlobal();
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"global_reset_cmd\":false}"); patchHttp.end();
    }

    if (doc.containsKey("sensor_test_cmd") && doc["sensor_test_cmd"].as<bool>() == true) {
      sysLog("Web Command: Sensor Test Display.");
      uiState = STATE_SENSOR_TEST;
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"sensor_test_cmd\":false}"); patchHttp.end();
    }

    if (doc.containsKey("tempLow") && tempLow != doc["tempLow"].as<float>()) { tempLow = doc["tempLow"]; changed = true; }
    if (doc.containsKey("tempHigh") && tempHigh != doc["tempHigh"].as<float>()) { tempHigh = doc["tempHigh"]; changed = true; }
    if (doc.containsKey("humLow") && humLow != doc["humLow"].as<float>()) { humLow = doc["humLow"]; changed = true; }
    if (doc.containsKey("humHigh") && humHigh != doc["humHigh"].as<float>()) { humHigh = doc["humHigh"]; changed = true; }
    if (doc.containsKey("soilLow") && soilLow != doc["soilLow"].as<float>()) { soilLow = doc["soilLow"]; changed = true; }
    if (doc.containsKey("soilHigh") && soilHigh != doc["soilHigh"].as<float>()) { soilHigh = doc["soilHigh"]; changed = true; }
    if (doc.containsKey("timeOnHour") && timeOnHour != doc["timeOnHour"].as<int>()) { timeOnHour = doc["timeOnHour"]; changed = true; }
    if (doc.containsKey("timeOffHour") && timeOffHour != doc["timeOffHour"].as<int>()) { timeOffHour = doc["timeOffHour"]; changed = true; }
    if (doc.containsKey("timeOnMinute") && timeOnMinute != doc["timeOnMinute"].as<int>()) { timeOnMinute = doc["timeOnMinute"]; changed = true; }
    if (doc.containsKey("timeOffMinute") && timeOffMinute != doc["timeOffMinute"].as<int>()) { timeOffMinute = doc["timeOffMinute"]; changed = true; }
    if (doc.containsKey("luxThreshold") && luxThreshold != doc["luxThreshold"].as<long>()) { luxThreshold = doc["luxThreshold"]; changed = true; }
    if (doc.containsKey("timerEnabled") && timerEnabled != doc["timerEnabled"].as<bool>()) { timerEnabled = doc["timerEnabled"]; changed = true; }
    if (doc.containsKey("timeZoneOffset") && timeZoneOffset != doc["timeZoneOffset"].as<int>()) { 
      timeZoneOffset = doc["timeZoneOffset"]; 
      configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
      changed = true; 
    }
    if (doc.containsKey("globalBrightness") && globalBrightness != doc["globalBrightness"].as<int>()) { 
      globalBrightness = doc["globalBrightness"]; 
      applyBrightness(globalBrightness); 
      changed = true; 
    }
    
    if (changed) { 
      sysLog("Cloud settings applied."); 
      updateBottomMenu("Web Update", "Received!"); 
      bottomMsgTimeout = millis() + 2000; 
      showingTempMsg = true; 
      externalUpdateReceived = true; 
    }
  }
  http.end();
}

void pushToCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  updateBottomMenu("Saving...", "To Cloud");
  HTTPClient http; http.begin(firebaseURL); http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<512> doc; 
  doc["tempLow"] = tempLow; doc["tempHigh"] = tempHigh; 
  doc["humLow"] = humLow; doc["humHigh"] = humHigh; 
  doc["soilLow"] = soilLow; doc["soilHigh"] = soilHigh; 
  doc["timeOnHour"] = timeOnHour; 
  doc["timeOffHour"] = timeOffHour;
  doc["timeOnMinute"] = timeOnMinute;
  doc["timeOffMinute"] = timeOffMinute;
  doc["luxThreshold"] = luxThreshold; 
  doc["timerEnabled"] = timerEnabled;
  doc["timeZoneOffset"] = timeZoneOffset;
  doc["globalBrightness"] = globalBrightness;
  
  String jsonOutput; serializeJson(doc, jsonOutput); int httpCode = http.sendRequest("PATCH", jsonOutput);
  if (httpCode > 0) updateBottomMenu("Cloud Update", "Successful!"); else updateBottomMenu("Cloud Error", String(httpCode));
  http.end(); delay(1000); lastMenuIdx = -1;
}

// ==========================================
// CORE 0: BACKGROUND CLOUD TASK
// ==========================================
void cloudTask(void * parameter) {
  for (;;) {
    // 1. If WiFi drops, just wait a bit and check again
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(2000 / portTICK_PERIOD_MS); 
      continue;
    }

    // 2. Check if the UI requested a settings push (User changed a setting)
    if (triggerCloudPush) {
      pushToCloud();
      triggerCloudPush = false;
    }

    // 3. Periodic Background Sync (Fetch settings + Push Live Data)
    static unsigned long lastCore0Sync = 0;
    if (millis() - lastCore0Sync > 10000) {
      lastCore0Sync = millis();
      
      // --- FIX: Check if the user is actively changing a setting on the screen ---
      bool userIsEditing = (uiState == STATE_EDIT_DUAL || 
                            uiState == STATE_EDIT_TIME || 
                            uiState == STATE_EDIT_LUX || 
                            uiState == STATE_EDIT_BRIGHTNESS);
      
      // Only fetch cloud settings if the user is safely in a normal menu
      if (!userIsEditing) {
        syncWithCloudSilent(); 
      }
      
      // Always push the live temperature/humidity/lux data to the dashboard
      pushLiveDataToCloud(); 
    }

    // Feed the watchdog timer so Core 0 doesn't crash
    vTaskDelay(100 / portTICK_PERIOD_MS); 
  }
}

// --- MASTER CONTROL LOGIC ---
void evaluateControlLogic() {
  if (WiFi.status() != WL_CONNECTED && millis() < 10000) return; // Wait for boot

  // 1. Peltier Heating/Cooling Logic
  if (liveTemp > tempHigh) {
    Serial2.println("CMD:COOL");
  } else if (liveTemp < tempLow) {
    Serial2.println("CMD:HEAT");
  } else {
    Serial2.println("CMD:PELTIER_OFF");
  }

  // 2. Light Control Logic
  bool turnLightOn = false;
  
  // Base condition: Turn on if it's too dark
  if (liveLux < luxThreshold) {
    turnLightOn = true;
  }

  // Timer condition: OVERRIDES everything else
  if (timerEnabled) {
    int nowMins = (currentHour * 60) + currentMinute;
    int onMins = (timeOnHour * 60) + timeOnMinute;
    int offMins = (timeOffHour * 60) + timeOffMinute;
    bool insideSchedule = false;
    
    if (onMins < offMins) {
      if (nowMins >= onMins && nowMins < offMins) insideSchedule = true;
    } else if (onMins > offMins) {
      if (nowMins >= onMins || nowMins < offMins) insideSchedule = true;
    }
    
    // If timer is on, but we are outside the hours, FORCE the light off
    if (!insideSchedule) {
      turnLightOn = false; 
    }
  }

  // 3. Send Light Command
  if (turnLightOn) {
    int pwmVal = map(globalBrightness, 1, 10, 25, 255);
    Serial2.print("CMD:LIGHT,");
    Serial2.println(pwmVal);
  } else {
    Serial2.println("CMD:LIGHT,0"); 
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  
  // INIT SERIAL FOR ARDUINO R4 COMMUNICATION
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  
  // Use interrupts for rotation (fast), but Polling for Button (reliable)
  pinMode(ENC_CLK, INPUT); 
  pinMode(ENC_DT, INPUT); 
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), readEncoder, CHANGE);
  
  Wire.begin(I2C_SDA, I2C_SCL);
  topDisplay.setBusClock(I2CBusClock); 
  bottomDisplay.setBusClock(I2CBusClock);
  bottomDisplay.setI2CAddress(BOTTOM_ADDR * 2); bottomDisplay.begin();
  topDisplay.setI2CAddress(TOP_ADDR * 2); topDisplay.begin();
  applyBrightness(globalBrightness);

  updateBottomMenu("Connecting...", "Checking WiFi");
  WiFi.mode(WIFI_STA);
  bool connected = false;
  int numNetworks = sizeof(myNetworks) / sizeof(myNetworks[0]);
  for (int i = 0; i < numNetworks; i++) {
    WiFi.disconnect(); 
    WiFi.begin(myNetworks[i].ssid, myNetworks[i].pass);
    for (int k = 0; k < 100; k++) { if (WiFi.status() == WL_CONNECTED) { connected = true; break; } delay(50); }
    if (connected) break;
  }

  if (connected) {
    updateBottomMenu("Connected!", WiFi.localIP().toString());
    configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
    syncWithCloudSilent();
  } else {
    updateBottomMenu("Offline Mode", "Starting UI...");
  }
  
  currentMenu = mainMenu; 
  currentMenuSize = 6; 
  lastMinuteTick = millis();

  // Create the Background Internet Task on Core 0
  xTaskCreatePinnedToCore(cloudTask, "CloudTask", 8192, NULL, 1, &CloudTaskHandle, 0);
}

void loop() {
  // 1. Process System Tasks
  updateClock();
  checkSerialSensors();
  
  static unsigned long lastLogicCheck = 0;
  if (millis() - lastLogicCheck > 5000) {
    lastLogicCheck = millis();
    evaluateControlLogic();
  }

  // 2. Encoder Rotation logic
  bool uiNeedsDraw = false;
  int rawEnc = encoderCount; 
  int currentEnc = rawEnc / 4; 
  int diff = lastEncoderCount - currentEnc;

  if (diff != 0) {
    lastEncoderCount = currentEnc; uiNeedsDraw = true; lastEncoderMoveTime = millis();
    if (uiState == STATE_MENU || uiState == STATE_SUBMENU || uiState == STATE_WIFI_SELECT) {
      selectedIndex += diff;
      if (selectedIndex < 0) selectedIndex = currentMenuSize - 1;
      if (selectedIndex >= currentMenuSize) selectedIndex = 0;
      if (selectedIndex < menuScrollOffset) menuScrollOffset = selectedIndex;
      if (selectedIndex >= menuScrollOffset + VISIBLE_ITEMS) menuScrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
    } else if (uiState == STATE_EDIT_DUAL) {
      editCurrent += diff;
      if (editStep == 0) { if (editCurrent < currentMinLimit) editCurrent = currentMinLimit; if (editCurrent > *pEditVal2) *pEditVal2 = editCurrent; }
      else { if (editCurrent < *pEditVal1) editCurrent = *pEditVal1; if (editCurrent > currentMaxLimit) editCurrent = currentMaxLimit; }
    } else if (uiState == STATE_EDIT_TIME) {
      editCurrent += diff; int limit = (editStep == 0 || editStep == 2 || editStep == 10) ? 23 : 59;
      if (editCurrent < 0) editCurrent = limit; if (editCurrent > limit) editCurrent = 0;
    } else if (uiState == STATE_EDIT_LUX) {
      long step = (abs(diff) > 1) ? 500 : 100; editCurrent += (diff * step); if (editCurrent < 0) editCurrent = 0;
    } else if (uiState == STATE_EDIT_BRIGHTNESS) {
      editCurrent += diff; if (editCurrent < 1) editCurrent = 1; if (editCurrent > 10) editCurrent = 10;
      globalBrightness = (int)editCurrent; applyBrightness(globalBrightness);
    }
  }

  // 3. --- RESTORED WORKING BUTTON POLLING ---
  static unsigned long lastBtnTime = 0; 
  bool clicked = false;
  
  if (digitalRead(ENC_SW) == LOW) { 
    if (millis() - lastBtnTime > 250) { 
      clicked = true; 
      lastBtnTime = millis(); 
    } 
  }

  // 4. Handle Clicks
  if (clicked) {
    uiNeedsDraw = true; lastEncoderMoveTime = millis();
    if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
      MenuItem& item = currentMenu[selectedIndex];
      if (String(item.name) == "Back") goBack();
      else if (item.children != nullptr) enterSubMenu(&item);
      else if (item.action != nullptr) item.action();
    } else if (uiState == STATE_WIFI_SELECT) {
      if (selectedIndex < sizeof(myNetworks) / sizeof(myNetworks[0])) attemptConnection(myNetworks[selectedIndex].ssid, myNetworks[selectedIndex].pass);
      else goBack();
    } else if (uiState == STATE_EDIT_DUAL) {
       if (editStep == 0) { *pEditVal1 = editCurrent; editStep = 1; editCurrent = *pEditVal2; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
       else { *pEditVal2 = editCurrent; updateBottomMenu("RANGE", "SAVED"); triggerCloudPush = true; goBack(); }
    } else if (uiState == STATE_EDIT_TIME) {
       if (editStep < 10) {
         if (editStep == 0) { tempOnH = (int)editCurrent; editStep = 1; editCurrent = tempOnM; } 
         else if (editStep == 1) { tempOnM = (int)editCurrent; editStep = 2; editCurrent = tempOffH; } 
         else if (editStep == 2) { tempOffH = (int)editCurrent; editStep = 3; editCurrent = tempOffM; } 
         else {
           tempOffM = (int)editCurrent; timeOnHour = tempOnH; timeOnMinute = tempOnM; timeOffHour = tempOffH; timeOffMinute = tempOffM;
           updateBottomMenu("SCHEDULE", "SAVED"); triggerCloudPush = true; goBack();
         }
         encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; delay(200);
       } else {
         if (editStep == 10) { currentHour = (int)editCurrent; editStep = 11; editCurrent = currentMinute; } 
         else { currentMinute = (int)editCurrent; updateBottomMenu("CLOCK", "UPDATED"); goBack(); }
         encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; delay(200);
       }
    } else if (uiState == STATE_EDIT_LUX) { luxThreshold = (long)editCurrent; updateBottomMenu("LUX LIMIT", "SAVED"); triggerCloudPush = true; goBack();
    } else if (uiState == STATE_EDIT_BRIGHTNESS) { updateBottomMenu("BRIGHTNESS", "SAVED"); triggerCloudPush = true; goBack();
    } else if (uiState == STATE_SENSOR_TEST) { goBack(); }
  }

  // 5. UI Drawing Logic
  static int lastMinDraw = -1;
  if (externalUpdateReceived || (currentMinute != lastMinDraw)) { uiNeedsDraw = true; externalUpdateReceived = false; lastMinDraw = currentMinute; }
  if (showingTempMsg && millis() > bottomMsgTimeout) { showingTempMsg = false; lastMenuIdx = -1; uiNeedsDraw = true; }

  static unsigned long lastBleUpdate = 0;
  if (uiState == STATE_SENSOR_TEST && millis() - lastBleUpdate > 1000) { uiNeedsDraw = true; lastBleUpdate = millis(); }

  if (uiNeedsDraw) {
    if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
      topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_6x10_tf);
      topDisplay.drawStr(getCenterX(topDisplay, currentHeaderName), 10, currentHeaderName.c_str());
      drawWiFiStatus(); topDisplay.drawHLine(0, 12, 128);
      for (int i = 0; i < currentMenuSize; i++) {
        if (i >= menuScrollOffset && i < menuScrollOffset + VISIBLE_ITEMS) {
          int y = 28 + ((i - menuScrollOffset) * 12);
          String label = currentMenu[i].name;
          if (label.startsWith("Timer")) label = timerEnabled ? "Timer: ON" : "Timer: OFF";

          if (i == selectedIndex) { 
            topDisplay.setFont(u8g2_font_7x14B_tf); topDisplay.drawStr(0, y, ">"); topDisplay.drawStr(10, y, label.c_str()); 
          } else { 
            topDisplay.setFont(u8g2_font_6x10_tf); topDisplay.drawStr(10, y, label.c_str()); 
          }
        }
      }
      topDisplay.sendBuffer();
      if (!showingTempMsg) { showHoverContext(currentMenu[selectedIndex].name); lastMenuIdx = selectedIndex; }
    } 
    else if (uiState == STATE_WIFI_SELECT) {
      topDisplay.clearBuffer(); topDisplay.setFont(u8g2_font_6x10_tf);
      topDisplay.drawStr(10, 10, "SELECT NETWORK"); topDisplay.drawHLine(0, 12, 128);
      for (int i = 0; i < currentMenuSize; i++) {
        if (i >= menuScrollOffset && i < menuScrollOffset + VISIBLE_ITEMS) {
           int y = 28 + ((i - menuScrollOffset) * 12);
           String label = (i < sizeof(myNetworks) / sizeof(myNetworks[0])) ? myNetworks[i].ssid : "Cancel";
           if (i == selectedIndex) { topDisplay.setFont(u8g2_font_7x14B_tf); topDisplay.drawStr(0, y, ">"); topDisplay.drawStr(10, y, label.c_str()); }
           else { topDisplay.setFont(u8g2_font_6x10_tf); topDisplay.drawStr(10, y, label.c_str()); }
        }
      }
      topDisplay.sendBuffer();
    } else if (uiState == STATE_EDIT_DUAL) {
       int valL = (editStep == 0) ? (int)editCurrent : (int)*pEditVal1;
       int valH = (editStep == 1) ? (int)editCurrent : (int)*pEditVal2;
       drawCircularGauge(valL, valH, currentMinLimit, currentMaxLimit, (editStep == 1));
       if (lastValDisp != editCurrent || lastStepDisp != editStep || (editStep == 0 && *pEditVal2 == editCurrent)) {
        if (editStep == 0) updateBottomEdit("SET LOWER", editCurrent, "High: ", *pEditVal2);
        else updateBottomEdit("SET UPPER", editCurrent, "Low: ", *pEditVal1);
        lastValDisp = editCurrent; lastStepDisp = editStep;
       }
    } else if (uiState == STATE_EDIT_TIME) {
       if(editStep < 10) {
         int showH = (editStep==0||editStep==2)?(int)editCurrent : (editStep==1?tempOnH:tempOffH);
         int showM = (editStep==1||editStep==3)?(int)editCurrent : (editStep==0?tempOnM:tempOffM);
         drawTimeEdit(showH, showM, (editStep==0||editStep==2), (editStep<2)?"TURN ON":"TURN OFF");
       } else {
         drawTimeEdit((editStep==10)?(int)editCurrent:currentHour, (editStep==11)?(int)editCurrent:currentMinute, (editStep==10), "SET CLOCK");
       }
    } else if (uiState == STATE_EDIT_LUX) drawLuxEdit((long)editCurrent);
    else if (uiState == STATE_EDIT_BRIGHTNESS) drawBrightnessEdit((int)editCurrent);
    else if (uiState == STATE_SENSOR_TEST) drawSensorTest();
  }
}