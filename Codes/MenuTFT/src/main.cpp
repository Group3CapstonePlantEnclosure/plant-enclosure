#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <FS.h>
#include <SPIFFS.h>

// --- FAST BOOT RTC VARIABLES ---
RTC_DATA_ATTR int rtc_bssid_valid = 0;
RTC_DATA_ATTR int rtc_ssid_index = -1;
RTC_DATA_ATTR uint8_t rtc_bssid[6];
RTC_DATA_ATTR uint8_t rtc_channel;

// --- DISPLAY & TOUCH LIBRARIES ---
#include <lvgl.h>
#include <TFT_eSPI.h> 

// --- SQUARELINE STUDIO UI ---
#include "ui.h"

// --- GLOBALS FOR DISPLAY (LVGL v8.3) ---
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf; // Pointer for PSRAM allocation

// --- UI POINTERS (Mapped to SquareLine in setup) ---
lv_obj_t * tabview;
lv_obj_t * temp_slider;
lv_obj_t * hum_slider;
lv_obj_t * soil_slider;
lv_obj_t * lux_slider;
lv_obj_t * temp_slider_label;
lv_obj_t * hum_slider_label;
lv_obj_t * soil_slider_label;
lv_obj_t * lux_slider_label;

lv_obj_t * unit_sw;
lv_obj_t * dm_sw;
lv_obj_t * timer_sw; 

// --- WIFI SETUP UI VARIABLES ---
lv_obj_t * wifi_list = NULL;
lv_obj_t * pwd_screen = NULL;
lv_obj_t * pwd_ta;
lv_obj_t * kb;
lv_obj_t * conn_status_label;
char selected_ssid[64];
char saved_ssid[64] = {0};
char saved_pass[64] = {0};
lv_obj_t * wifi_kb_toggle_btn = NULL;
lv_obj_t * wifi_kb_toggle_label = NULL;
lv_obj_t * wifi_pwd_eye_btn = NULL;
lv_obj_t * wifi_pwd_eye_label = NULL;
bool wifi_keyboard_hidden = false;
Preferences wifiPrefs;

// --- TIME PICKER UI VARIABLES ---
lv_obj_t * time_picker_bg = NULL;
lv_obj_t * roller_h;
lv_obj_t * roller_m;
lv_obj_t * roller_ampm;
bool isEditingOnTime = true;
lv_obj_t * lbl_time_on;
lv_obj_t * lbl_time_off;

void build_wifi_scanner_ui(); 
void open_time_picker(bool isOnTime);
void build_wifi_password_ui(const char *ssid);
void close_wifi_overlays(bool restoreTabview);
void load_wifi_credentials();
void save_wifi_credentials(const char *ssid, const char *password);
bool connect_to_wifi(const char *ssid, const char *password, bool useFastReconnect);
bool patch_firebase_payload(const String &payload);
bool push_settings_to_firebase();
bool push_live_values_to_firebase();
void load_settings_from_firebase();
void cloudTask(void * parameter);
void checkSerialSensors();
void updateClock();
void evaluateControlLogic();
void fix_layout_for_display();

// --- PIN DEFINITIONS ---
#define TOUCH_IRQ  16  
#define SD_CS      5
// #define TFT_BL  38  // REMOVED: Pin 38 is reserved for PSRAM on N16R8

#ifndef RXD2
#define RXD2 15
#endif
#ifndef TXD2
#define TXD2 17
#endif

// Backlight PWM Settings (Disabled for initial test)
const int pwmFreq = 5000;
const int pwmResolution = 8;
const int pwmChannel = 0;
volatile bool isAsleep = false;

TFT_eSPI tft = TFT_eSPI();

// 1. Update Resolution
static const uint16_t screenWidth  = 480; 
static const uint16_t screenHeight = 320;

// --- SYSTEM GLOBALS ---
float liveTemp = 0.0, liveHum = 0.0, liveSoil = 0.0, liveLux = 0.0;
float tempLow = 68.0, tempHigh = 77.0;
float humLow = 40.0, humHigh = 80.0;
float soilLow = 30.0, soilHigh = 70.0;
float luxThreshold = 30000;

bool useFahrenheit = true; 
bool isDarkMode = true; 
bool timerEnabled = false;
int timeOnHour = 8, timeOnMinute = 0, timeOffHour = 20, timeOffMinute = 0;
int currentHour = 12, currentMinute = 0, globalBrightness = 10;
int timeZoneOffset = -5; 

String webLogBuffer = "Device Booted. System initialized.\n";
unsigned long lastMinuteTick = 0, lastSerialRecv = 0, lastControlCheck = 0;
String lastPeltierCommand = "";
int lastLightPwm = -1;

volatile bool triggerCloudPush = false;
volatile bool uiNeedsUpdate = false; 
volatile bool isScanning = false;
volatile bool isConnecting = false; 

TaskHandle_t CloudTaskHandle = NULL;
TaskHandle_t UILoopTaskHandle = NULL; 

const char* firebaseURL = "https://plant-enclosure-default-rtdb.firebaseio.com/settings.json";

struct WiFiCreds { const char* ssid; const char* pass; };
WiFiCreds myNetworks[] = {
  { "MASCU-Fi", "milliondollarbackyard@1192" },
  { "shanel", "jamescharles" }                  
};

const char * mins_str = "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";
const char * hrs_str = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12";
const char * ampm_str = "AM\nPM";

// ==========================================
// UTILS & LOGGING
// ==========================================

void sysLog(String msg) {
  Serial.println(msg);
  String timeStr = "[" + String(currentHour) + ":" + (currentMinute < 10 ? "0" : "") + String(currentMinute) + "] ";
  webLogBuffer += timeStr + msg + "\n";
  if (webLogBuffer.length() > 1500) webLogBuffer = webLogBuffer.substring(webLogBuffer.length() - 1000);
}

void close_wifi_overlays(bool restoreTabview) {
  if(pwd_screen != NULL) {
    lv_obj_del(pwd_screen);
    pwd_screen = NULL;
    pwd_ta = NULL;
    kb = NULL;
    conn_status_label = NULL;
    wifi_kb_toggle_btn = NULL;
    wifi_kb_toggle_label = NULL;
    wifi_keyboard_hidden = false;
  }

  if(restoreTabview && wifi_list != NULL) {
    lv_obj_del(wifi_list);
    wifi_list = NULL;
  }

  if(restoreTabview && tabview != NULL) {
    lv_obj_clear_flag(tabview, LV_OBJ_FLAG_HIDDEN);
  }
}

void load_wifi_credentials() {
  saved_ssid[0] = '\0';
  saved_pass[0] = '\0';

  wifiPrefs.begin("wifi", true);
  String storedSsid = wifiPrefs.getString("ssid", "");
  String storedPass = wifiPrefs.getString("pass", "");
  wifiPrefs.end();

  snprintf(saved_ssid, sizeof(saved_ssid), "%s", storedSsid.c_str());
  snprintf(saved_pass, sizeof(saved_pass), "%s", storedPass.c_str());
}

void save_wifi_credentials(const char *ssid, const char *password) {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", ssid != NULL ? ssid : "");
  wifiPrefs.putString("pass", password != NULL ? password : "");
  wifiPrefs.end();

  snprintf(saved_ssid, sizeof(saved_ssid), "%s", ssid != NULL ? ssid : "");
  snprintf(saved_pass, sizeof(saved_pass), "%s", password != NULL ? password : "");
}

bool connect_to_wifi(const char *ssid, const char *password, bool useFastReconnect) {
  if(ssid == NULL || ssid[0] == '\0') return false;

  isConnecting = true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if(conn_status_label) lv_label_set_text_fmt(conn_status_label, "Connecting to %s...", ssid);
  sysLog("Connecting to " + String(ssid));

  if(WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(ssid)) {
    isConnecting = false;
    if(conn_status_label) lv_label_set_text(conn_status_label, "Already connected.");
    return true;
  }

  WiFi.disconnect(false, false);
  delay(100);

  if(useFastReconnect && rtc_bssid_valid) {
    WiFi.begin(ssid, password, rtc_channel, rtc_bssid, true);
  } else {
    WiFi.begin(ssid, password);
  }

  unsigned long start = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  isConnecting = false;

  if(WiFi.status() == WL_CONNECTED) {
    const uint8_t *bssid = WiFi.BSSID();
    if(bssid != NULL) {
      memcpy(rtc_bssid, bssid, sizeof(rtc_bssid));
      rtc_channel = WiFi.channel();
      rtc_bssid_valid = 1;
    }
    configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
    lastMinuteTick = 0;
    sysLog("WiFi connected. IP: " + WiFi.localIP().toString());
    if(conn_status_label) lv_label_set_text(conn_status_label, "Connected successfully.");
    uiNeedsUpdate = true;
    return true;
  }

  rtc_bssid_valid = 0;
  sysLog("WiFi connection failed for " + String(ssid));
  if(conn_status_label) lv_label_set_text(conn_status_label, "Connection failed. Check the password.");
  return false;
}

bool patch_firebase_payload(const String &payload) {
  if(WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if(!http.begin(client, firebaseURL)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.sendRequest("PATCH", payload);
  http.end();

  return httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT;
}

bool push_settings_to_firebase() {
  if(WiFi.status() != WL_CONNECTED) {
    sysLog("Skipping Firebase push because WiFi is not connected.");
    return false;
  }

  const float tempLowCloud = useFahrenheit ? tempLow : ((tempLow * 9.0f / 5.0f) + 32.0f);
  const float tempHighCloud = useFahrenheit ? tempHigh : ((tempHigh * 9.0f / 5.0f) + 32.0f);

  DynamicJsonDocument doc(2048);
  doc["tempLow"] = tempLowCloud;
  doc["tempHigh"] = tempHighCloud;
  doc["humLow"] = humLow;
  doc["humHigh"] = humHigh;
  doc["soilLow"] = soilLow;
  doc["soilHigh"] = soilHigh;
  doc["luxThreshold"] = luxThreshold;
  doc["useFahrenheit"] = useFahrenheit;
  doc["darkMode"] = isDarkMode;
  doc["isDarkMode"] = isDarkMode;
  doc["timerEnabled"] = timerEnabled;
  doc["timeOnHour"] = timeOnHour;
  doc["timeOnMin"] = timeOnMinute;
  doc["timeOffHour"] = timeOffHour;
  doc["timeOffMin"] = timeOffMinute;
  doc["globalBrightness"] = globalBrightness;
  doc["timeZoneOffset"] = timeZoneOffset;

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if(!http.begin(client, firebaseURL)) {
    sysLog("Unable to start Firebase PATCH request.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.sendRequest("PATCH", payload);
  http.end();

  if(httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT) {
    sysLog("Firebase settings pushed.");
    return true;
  }

  sysLog("Firebase PATCH failed with code " + String(httpCode));
  return false;
}

bool push_live_values_to_firebase() {
  if(WiFi.status() != WL_CONNECTED) return false;
  if(lastSerialRecv == 0 || millis() - lastSerialRecv > 30000UL) return false;

  const float liveTempCloud = useFahrenheit ? liveTemp : ((liveTemp * 9.0f / 5.0f) + 32.0f);

  DynamicJsonDocument doc(512);
  doc["liveTemp"] = liveTempCloud;
  doc["currentTemp"] = liveTempCloud;
  doc["liveHum"] = liveHum;
  doc["currentHumidity"] = liveHum;
  doc["liveSoil"] = liveSoil;
  doc["soilMoisture"] = liveSoil;
  doc["liveLux"] = liveLux;
  doc["currentLux"] = liveLux;

  String payload;
  serializeJson(doc, payload);

  if(patch_firebase_payload(payload)) {
    return true;
  }

  sysLog("Live sensor upload failed.");
  return false;
}

void load_settings_from_firebase() {
  if(WiFi.status() != WL_CONNECTED) {
    sysLog("Skipping Firebase sync because WiFi is not connected.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if(!http.begin(client, firebaseURL)) {
    sysLog("Unable to start Firebase request.");
    return;
  }

  int httpCode = http.GET();
  if(httpCode != HTTP_CODE_OK) {
    sysLog("Firebase GET failed with code " + String(httpCode));
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if(err) {
    sysLog("Firebase JSON parse failed.");
    return;
  }

  bool changed = false;

  if(doc["reboot_cmd"] == true) {
    sysLog("Web Command: Remote Reboot...");
    patch_firebase_payload("{\"reboot_cmd\":false}");
    delay(1000);
    ESP.restart();
    return;
  }

  if(doc["global_reset_cmd"] == true) {
    sysLog("Web Command: Factory Reset...");
    tempLow = 68.0f; tempHigh = 77.0f;
    humLow = 40.0f; humHigh = 80.0f;
    soilLow = 30.0f; soilHigh = 70.0f;
    luxThreshold = 30000.0f;
    timeOnHour = 8; timeOnMinute = 0;
    timeOffHour = 20; timeOffMinute = 0;
    timerEnabled = false;
    globalBrightness = 10;
    isDarkMode = true;
    patch_firebase_payload("{\"global_reset_cmd\":false}");
    triggerCloudPush = true;
    changed = true;
  }

  if(doc["sensor_test_cmd"] == true) {
    sysLog("Web Command: Sensor test active.");
    if(tabview) lv_tabview_set_act(tabview, 0, LV_ANIM_ON);
    patch_firebase_payload("{\"sensor_test_cmd\":false}");
  }

  if(doc["fetch_logs_cmd"] == true) {
    DynamicJsonDocument logDoc(3072);
    logDoc["fetch_logs_cmd"] = false;
    logDoc["system_logs"] = webLogBuffer;
    String logPayload;
    serializeJson(logDoc, logPayload);
    patch_firebase_payload(logPayload);
  }

  if(!doc["useFahrenheit"].isNull() && useFahrenheit != doc["useFahrenheit"].as<bool>()) {
    useFahrenheit = doc["useFahrenheit"].as<bool>();
    changed = true;
  }

  bool newDarkMode = isDarkMode;
  if(!doc["darkMode"].isNull()) newDarkMode = doc["darkMode"].as<bool>();
  else if(!doc["isDarkMode"].isNull()) newDarkMode = doc["isDarkMode"].as<bool>();
  if(newDarkMode != isDarkMode) {
    isDarkMode = newDarkMode;
    changed = true;
  }

  if(!doc["timerEnabled"].isNull() && timerEnabled != doc["timerEnabled"].as<bool>()) {
    timerEnabled = doc["timerEnabled"].as<bool>();
    changed = true;
  }
  if(!doc["timeOnHour"].isNull() && timeOnHour != doc["timeOnHour"].as<int>()) { timeOnHour = doc["timeOnHour"].as<int>(); changed = true; }
  if(!doc["timeOnMin"].isNull() && timeOnMinute != doc["timeOnMin"].as<int>()) { timeOnMinute = doc["timeOnMin"].as<int>(); changed = true; }
  if(!doc["timeOffHour"].isNull() && timeOffHour != doc["timeOffHour"].as<int>()) { timeOffHour = doc["timeOffHour"].as<int>(); changed = true; }
  if(!doc["timeOffMin"].isNull() && timeOffMinute != doc["timeOffMin"].as<int>()) { timeOffMinute = doc["timeOffMin"].as<int>(); changed = true; }
  if(!doc["globalBrightness"].isNull() && globalBrightness != doc["globalBrightness"].as<int>()) { globalBrightness = doc["globalBrightness"].as<int>(); changed = true; }
  if(!doc["timeZoneOffset"].isNull() && timeZoneOffset != doc["timeZoneOffset"].as<int>()) {
    timeZoneOffset = doc["timeZoneOffset"].as<int>();
    configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
    sysLog("Timezone updated to UTC " + String(timeZoneOffset));
    changed = true;
  }

  if(!doc["tempLow"].isNull()) {
    float v = doc["tempLow"].as<float>();
    float newVal = useFahrenheit ? v : ((v - 32.0f) * 5.0f / 9.0f);
    if(fabsf(newVal - tempLow) > 0.05f) { tempLow = newVal; changed = true; }
  } else if(!doc["temperature"]["low"].isNull()) {
    float v = doc["temperature"]["low"].as<float>();
    float newVal = useFahrenheit ? v : ((v - 32.0f) * 5.0f / 9.0f);
    if(fabsf(newVal - tempLow) > 0.05f) { tempLow = newVal; changed = true; }
  }

  if(!doc["tempHigh"].isNull()) {
    float v = doc["tempHigh"].as<float>();
    float newVal = useFahrenheit ? v : ((v - 32.0f) * 5.0f / 9.0f);
    if(fabsf(newVal - tempHigh) > 0.05f) { tempHigh = newVal; changed = true; }
  } else if(!doc["temperature"]["high"].isNull()) {
    float v = doc["temperature"]["high"].as<float>();
    float newVal = useFahrenheit ? v : ((v - 32.0f) * 5.0f / 9.0f);
    if(fabsf(newVal - tempHigh) > 0.05f) { tempHigh = newVal; changed = true; }
  }

  if(!doc["humLow"].isNull() && fabsf(doc["humLow"].as<float>() - humLow) > 0.05f) { humLow = doc["humLow"].as<float>(); changed = true; }
  else if(!doc["humidity"]["low"].isNull() && fabsf(doc["humidity"]["low"].as<float>() - humLow) > 0.05f) { humLow = doc["humidity"]["low"].as<float>(); changed = true; }

  if(!doc["humHigh"].isNull() && fabsf(doc["humHigh"].as<float>() - humHigh) > 0.05f) { humHigh = doc["humHigh"].as<float>(); changed = true; }
  else if(!doc["humidity"]["high"].isNull() && fabsf(doc["humidity"]["high"].as<float>() - humHigh) > 0.05f) { humHigh = doc["humidity"]["high"].as<float>(); changed = true; }

  if(!doc["soilLow"].isNull() && fabsf(doc["soilLow"].as<float>() - soilLow) > 0.05f) { soilLow = doc["soilLow"].as<float>(); changed = true; }
  else if(!doc["soil"]["low"].isNull() && fabsf(doc["soil"]["low"].as<float>() - soilLow) > 0.05f) { soilLow = doc["soil"]["low"].as<float>(); changed = true; }
  else if(!doc["moisture"]["low"].isNull() && fabsf(doc["moisture"]["low"].as<float>() - soilLow) > 0.05f) { soilLow = doc["moisture"]["low"].as<float>(); changed = true; }

  if(!doc["soilHigh"].isNull() && fabsf(doc["soilHigh"].as<float>() - soilHigh) > 0.05f) { soilHigh = doc["soilHigh"].as<float>(); changed = true; }
  else if(!doc["soil"]["high"].isNull() && fabsf(doc["soil"]["high"].as<float>() - soilHigh) > 0.05f) { soilHigh = doc["soil"]["high"].as<float>(); changed = true; }
  else if(!doc["moisture"]["high"].isNull() && fabsf(doc["moisture"]["high"].as<float>() - soilHigh) > 0.05f) { soilHigh = doc["moisture"]["high"].as<float>(); changed = true; }

  if(!doc["luxThreshold"].isNull() && fabsf(doc["luxThreshold"].as<float>() - luxThreshold) > 0.5f) { luxThreshold = doc["luxThreshold"].as<float>(); changed = true; }
  else if(!doc["lighting"]["luxThreshold"].isNull() && fabsf(doc["lighting"]["luxThreshold"].as<float>() - luxThreshold) > 0.5f) { luxThreshold = doc["lighting"]["luxThreshold"].as<float>(); changed = true; }

  // Live UART sensor values are source-of-truth on ESP32 and should not be overwritten from cloud sync.

  if(changed) {
    sysLog("Cloud settings synced to device.");
    uiNeedsUpdate = true;
  }
}

void cloudTask(void * parameter) {
  unsigned long lastSettingsSyncMs = 0;
  unsigned long lastLiveUploadMs = 0;
  (void)parameter;

  for(;;) {
    if(isScanning || isConnecting) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if(WiFi.status() != WL_CONNECTED && saved_ssid[0] != '\0') {
      connect_to_wifi(saved_ssid, saved_pass, true);
    }

    if(WiFi.status() == WL_CONNECTED) {
      if(triggerCloudPush) {
        if(push_settings_to_firebase()) {
          triggerCloudPush = false;
          lastSettingsSyncMs = millis();
        }
      }

      if(millis() - lastSettingsSyncMs >= 10000UL) {
        load_settings_from_firebase();
        lastSettingsSyncMs = millis();
      }

      if(millis() - lastLiveUploadMs >= 10000UL) {
        push_live_values_to_firebase();
        lastLiveUploadMs = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void fix_layout_for_display() {
  if(ui_Screen1) lv_obj_set_size(ui_Screen1, screenWidth, screenHeight);

  if(ui_status_bar) {
    lv_obj_set_size(ui_status_bar, screenWidth, 30);
    lv_obj_align(ui_status_bar, LV_ALIGN_TOP_MID, 0, 0);
  }

  if(ui_TabView1) {
    lv_obj_set_size(ui_TabView1, screenWidth, screenHeight - 30);
    lv_obj_align(ui_TabView1, LV_ALIGN_BOTTOM_MID, 0, 0);
  }
}

// ==========================================
// CALIBRATION ROUTINE
// ==========================================
void touch_calibrate() {
  return; // BYPASS FOR NOW

  uint16_t calibrationData[5];
  uint8_t calDataOK = 0;

  if (!SPIFFS.begin()) {
    SPIFFS.format();
    SPIFFS.begin();
  }

  if (SPIFFS.exists("/TouchCalData")) {
    File f = SPIFFS.open("/TouchCalData", "r");
    if (f) {
      if (f.readBytes((char *)calibrationData, 14) == 14) calDataOK = 1;
      f.close();
    }
  }

  if (calDataOK) {
    // tft.setTouch(calibrationData); // COMMENTED TO STOP ERRORS
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(20, 0);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("Touch corners as indicated");
    // tft.calibrateTouch(calibrationData, TFT_MAGENTA, TFT_BLACK, 15); // COMMENTED TO STOP ERRORS
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("Calibration complete!");
    File f = SPIFFS.open("/TouchCalData", "w");
    if (f) {
      f.write((const unsigned char *)calibrationData, 14);
      f.close();
    }
  }
}

// ==========================================
// LVGL v8 CALLBACKS 
// ==========================================
void IRAM_ATTR handleTouchInterrupt() {
  if (isAsleep) isAsleep = false; 
}

void setBacklight(int brightness) {
  // ledcWrite(pwmChannel, brightness); 
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
  lv_disp_flush_ready(disp);
}

/* Read the touchpad for LVGL */
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data)
{
    uint16_t touchX = 0, touchY = 0;

    // Check if the screen is being touched
    bool touched = tft.getTouch(&touchX, &touchY);

    if (!touched) {
        data->state = LV_INDEV_STATE_REL; // Released
    } else {
        data->state = LV_INDEV_STATE_PR;  // Pressed
        
        // Pass the coordinates to LVGL
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

// ==========================================
// SQUARELINE C++ BRIDGES
// ==========================================

void global_back_cb(lv_event_t * e) {
    if(ui_env_page != NULL) lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_HIDDEN);
    if(ui_lighting_page != NULL) lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_HIDDEN);
    if(ui_display_page != NULL) lv_obj_add_flag(ui_display_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    if(ui_menu_list != NULL) lv_obj_clear_flag(ui_menu_list, LV_OBJ_FLAG_HIDDEN);
}

void open_wifi_scanner_cb(lv_event_t * e) {
  sysLog("Opening C++ WiFi Scanner...");
  build_wifi_scanner_ui(); 
}

// ==========================================
// CONTROL CALLBACKS
// ==========================================

void temp_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    float sliderLow = lv_slider_get_left_value(slider);
    float sliderHigh = lv_slider_get_value(slider);
    tempLow = useFahrenheit ? sliderLow : (sliderLow * 9.0f / 5.0f + 32.0f);
    tempHigh = useFahrenheit ? sliderHigh : (sliderHigh * 9.0f / 5.0f + 32.0f);
    char buf[64]; snprintf(buf, sizeof(buf), "Temperature: %.0f - %.0f \xb0%s", sliderLow, sliderHigh, useFahrenheit ? "F" : "C");
    if(temp_slider_label) lv_label_set_text(temp_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

void hum_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    humLow = lv_slider_get_left_value(slider);
    humHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Humidity: %.0f - %.0f %%", humLow, humHigh);
    if(hum_slider_label) lv_label_set_text(hum_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

void soil_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    soilLow = lv_slider_get_left_value(slider);
    soilHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Soil Moisture: %.0f - %.0f %%", soilLow, soilHigh);
    if(soil_slider_label) lv_label_set_text(soil_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true;
}

void dark_mode_switch_cb(lv_event_t * e) {
  lv_obj_t * sw = lv_event_get_target(e);
  isDarkMode = lv_obj_has_state(sw, LV_STATE_CHECKED);
  triggerCloudPush = true;
}

// ==========================================
// WIFI & TIME PICKER UI 
// ==========================================

static void wifi_network_btn_cb(lv_event_t * e) {
  lv_obj_t * btn = lv_event_get_target(e);
  const char *ssid = lv_list_get_btn_text(wifi_list, btn);
  if(ssid == NULL || ssid[0] == '\0') return;
  build_wifi_password_ui(ssid);
}

static void wifi_connect_now() {
  if(pwd_ta == NULL || selected_ssid[0] == '\0') return;

  const char *password = lv_textarea_get_text(pwd_ta);
  if(password == NULL || strlen(password) == 0) {
    if(conn_status_label) lv_label_set_text(conn_status_label, "Password cannot be empty.");
    return;
  }

  if(conn_status_label) lv_label_set_text(conn_status_label, "Connecting...");

  if(connect_to_wifi(selected_ssid, password, true)) {
    save_wifi_credentials(selected_ssid, password);
    load_settings_from_firebase();
    close_wifi_overlays(true);
  }
}

static void wifi_keyboard_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);

  if(code == LV_EVENT_READY) {
    wifi_connect_now();
  } else if(code == LV_EVENT_CANCEL) {
    if(kb != NULL) {
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
      wifi_keyboard_hidden = true;
      if(wifi_kb_toggle_label) lv_label_set_text(wifi_kb_toggle_label, "Show KB");
    }
  }
}

static void wifi_textarea_event_cb(lv_event_t * e) {
  if(lv_event_get_code(e) == LV_EVENT_CLICKED && kb != NULL) {
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    wifi_keyboard_hidden = false;
    if(wifi_kb_toggle_label) lv_label_set_text(wifi_kb_toggle_label, "Hide KB");
    _ui_keyboard_set_target(kb, pwd_ta);
  }
}

static void wifi_cancel_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  close_wifi_overlays(false);
}

static void wifi_toggle_password_eye_cb(lv_event_t * e) {
  LV_UNUSED(e);
  if(pwd_ta == NULL || wifi_pwd_eye_label == NULL) return;

  bool is_pwd = lv_textarea_get_password_mode(pwd_ta);
  lv_textarea_set_password_mode(pwd_ta, !is_pwd);
  lv_label_set_text(wifi_pwd_eye_label, is_pwd ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void wifi_toggle_keyboard_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  if(kb == NULL || wifi_kb_toggle_label == NULL) return;

  wifi_keyboard_hidden = !wifi_keyboard_hidden;
  if(wifi_keyboard_hidden) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_kb_toggle_label, "Show KB");
  } else {
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_kb_toggle_label, "Hide KB");
    _ui_keyboard_set_target(kb, pwd_ta);
  }
}

void build_wifi_password_ui(const char *ssid) {
  if(ssid == NULL || ssid[0] == '\0') return;

  snprintf(selected_ssid, sizeof(selected_ssid), "%s", ssid);
  close_wifi_overlays(false);

  pwd_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(pwd_screen, screenWidth, screenHeight);
  lv_obj_center(pwd_screen);
  lv_obj_set_style_bg_color(pwd_screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(pwd_screen, LV_OPA_70, 0);
  lv_obj_set_style_border_width(pwd_screen, 0, 0);
  lv_obj_clear_flag(pwd_screen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * modal = lv_obj_create(pwd_screen);
  lv_obj_set_size(modal, screenWidth - 28, screenHeight - 18);
  lv_obj_center(modal);
  lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(modal, 8, 0);

  lv_obj_t * back_btn = lv_btn_create(modal);
  lv_obj_set_size(back_btn, 52, 32);
  lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t * back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, wifi_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t * title = lv_label_create(modal);
  lv_label_set_text(title, "Connect to WiFi");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t * ssid_label = lv_label_create(modal);
  lv_obj_set_width(ssid_label, screenWidth - 80);
  lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(ssid_label, "SSID: %s", ssid);
  lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 0, 42);

  pwd_ta = lv_textarea_create(modal);
  lv_obj_set_size(pwd_ta, screenWidth - 110, 40);
  lv_obj_align(pwd_ta, LV_ALIGN_TOP_LEFT, 0, 86);
  lv_textarea_set_placeholder_text(pwd_ta, "Enter WiFi password");
  lv_textarea_set_password_mode(pwd_ta, true);
  lv_textarea_set_one_line(pwd_ta, true);
  lv_obj_add_event_cb(pwd_ta, wifi_textarea_event_cb, LV_EVENT_CLICKED, NULL);

  if(strcmp(saved_ssid, ssid) == 0 && saved_pass[0] != '\0') {
    lv_textarea_set_text(pwd_ta, saved_pass);
  }

  wifi_pwd_eye_btn = lv_btn_create(modal);
  lv_obj_set_size(wifi_pwd_eye_btn, 46, 40);
  lv_obj_align_to(wifi_pwd_eye_btn, pwd_ta, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
  wifi_pwd_eye_label = lv_label_create(wifi_pwd_eye_btn);
  lv_label_set_text(wifi_pwd_eye_label, LV_SYMBOL_EYE_OPEN);
  lv_obj_center(wifi_pwd_eye_label);
  lv_obj_add_event_cb(wifi_pwd_eye_btn, wifi_toggle_password_eye_cb, LV_EVENT_CLICKED, NULL);

  conn_status_label = lv_label_create(modal);
  lv_obj_set_width(conn_status_label, screenWidth - 80);
  lv_label_set_long_mode(conn_status_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(conn_status_label, "Tap the password field to show the keyboard. Use the keyboard checkmark to connect.");
  lv_obj_align(conn_status_label, LV_ALIGN_TOP_LEFT, 0, 136);

  wifi_kb_toggle_btn = lv_btn_create(modal);
  lv_obj_set_size(wifi_kb_toggle_btn, 96, 34);
  lv_obj_align(wifi_kb_toggle_btn, LV_ALIGN_TOP_RIGHT, 0, 174);
  wifi_kb_toggle_label = lv_label_create(wifi_kb_toggle_btn);
  lv_label_set_text(wifi_kb_toggle_label, "Show KB");
  lv_obj_center(wifi_kb_toggle_label);
  lv_obj_add_event_cb(wifi_kb_toggle_btn, wifi_toggle_keyboard_btn_cb, LV_EVENT_CLICKED, NULL);

  kb = lv_keyboard_create(modal);
  lv_obj_set_size(kb, screenWidth - 54, 122);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(kb, wifi_keyboard_event_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb, wifi_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
  _ui_keyboard_set_target(kb, pwd_ta);

  wifi_keyboard_hidden = true;
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void build_wifi_scanner_ui() {
  if(wifi_list != NULL) {
    lv_obj_del(wifi_list);
    wifi_list = NULL;
  }
  if(tabview != NULL) lv_obj_add_flag(tabview, LV_OBJ_FLAG_HIDDEN);
  if(CloudTaskHandle != NULL) vTaskSuspend(CloudTaskHandle);

  isScanning = true;
  sysLog("Scanning WiFi networks...");
  int n = WiFi.scanNetworks(false, false);
  isScanning = false;

  if(CloudTaskHandle != NULL) vTaskResume(CloudTaskHandle);

  wifi_list = lv_list_create(lv_scr_act());
  lv_obj_set_size(wifi_list, screenWidth - 28, screenHeight - 82);
  lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_obj_t * back_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_LEFT, "Back to Settings");
  lv_obj_add_event_cb(back_btn, [](lv_event_t * e){
    LV_UNUSED(e);
    close_wifi_overlays(true);
  }, LV_EVENT_CLICKED, NULL);

  lv_list_add_text(wifi_list, "Available Networks");
  if(n <= 0) {
    lv_list_add_btn(wifi_list, LV_SYMBOL_REFRESH, "No visible networks found.");
  } else {
    for(int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      if(ssid.length() == 0) continue;

      lv_obj_t * btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, ssid.c_str());
      lv_obj_add_event_cb(btn, wifi_network_btn_cb, LV_EVENT_CLICKED, NULL);
    }
  }

  WiFi.scanDelete();
}

void build_time_picker_modal() {
  time_picker_bg = lv_obj_create(lv_scr_act());
  lv_obj_set_size(time_picker_bg, screenWidth, screenHeight);
  lv_obj_center(time_picker_bg);
  lv_obj_set_style_bg_color(time_picker_bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(time_picker_bg, LV_OPA_80, 0);
  lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t * modal = lv_obj_create(time_picker_bg);
  lv_obj_set_size(modal, screenWidth - 80, screenHeight - 80);
  lv_obj_center(modal);

  lv_obj_t * title = lv_label_create(modal);
  lv_obj_set_user_data(time_picker_bg, title);
  lv_label_set_text(title, "SELECT TIME");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

  lv_obj_t * cancel_btn = lv_btn_create(modal);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
  lv_obj_t * cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, "CANCEL");
  lv_obj_add_event_cb(cancel_btn, [](lv_event_t * e){ LV_UNUSED(e); lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// BACKGROUND ARDUINO & CLOUD TASKS
// ==========================================

void update_ui_from_data() {
  if(!uiNeedsUpdate) return;
  uiNeedsUpdate = false;
  char buf_txt[64];

  if(ui_temp_label) {
    snprintf(buf_txt, sizeof(buf_txt), "Temp: %.1f \xb0%s", liveTemp, useFahrenheit ? "F" : "C");
    lv_label_set_text(ui_temp_label, buf_txt);
  }
  if(ui_hum_label) {
    snprintf(buf_txt, sizeof(buf_txt), "Humidity: %.0f %%", liveHum);
    lv_label_set_text(ui_hum_label, buf_txt);
  }
  if(ui_moisture_label) {
    snprintf(buf_txt, sizeof(buf_txt), "Moisture: %.0f %%", liveSoil);
    lv_label_set_text(ui_moisture_label, buf_txt);
  }
  if(ui_lux_label) {
    snprintf(buf_txt, sizeof(buf_txt), "Light: %.0f lux", liveLux);
    lv_label_set_text(ui_lux_label, buf_txt);
  }

  // UART status indicator on dashboard
  static lv_obj_t * uart_status_label = NULL;
  if(uart_status_label == NULL && ui_Dashboard != NULL) {
    uart_status_label = lv_label_create(ui_Dashboard);
    lv_obj_align(uart_status_label, LV_ALIGN_BOTTOM_LEFT, 5, -5);
  }
  if(uart_status_label) {
    if(lastSerialRecv == 0) {
      lv_label_set_text(uart_status_label, "UART: No Data");
      lv_obj_set_style_text_color(uart_status_label, lv_color_hex(0xFF0000), 0);
    } else if(millis() - lastSerialRecv > 15000UL) {
      snprintf(buf_txt, sizeof(buf_txt), "UART: Stale (%lus ago)", (millis() - lastSerialRecv) / 1000UL);
      lv_label_set_text(uart_status_label, buf_txt);
      lv_obj_set_style_text_color(uart_status_label, lv_color_hex(0xFFA500), 0);
    } else {
      snprintf(buf_txt, sizeof(buf_txt), "UART: OK (%lus ago)", (millis() - lastSerialRecv) / 1000UL);
      lv_label_set_text(uart_status_label, buf_txt);
      lv_obj_set_style_text_color(uart_status_label, lv_color_hex(0x00FF00), 0);
    }
  }

  if(ui_wifistatuslabel) {
    if(WiFi.status() == WL_CONNECTED) {
      lv_label_set_text(ui_wifistatuslabel, LV_SYMBOL_WIFI);
      lv_obj_set_style_text_color(ui_wifistatuslabel, lv_color_hex(0x00FF00), 0);
    } else {
      lv_label_set_text(ui_wifistatuslabel, LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_color(ui_wifistatuslabel, lv_color_hex(0xFF0000), 0);
    }
  }

  if(temp_slider) {
    float displayLow = useFahrenheit ? tempLow : ((tempLow - 32.0f) * 5.0f / 9.0f);
    float displayHigh = useFahrenheit ? tempHigh : ((tempHigh - 32.0f) * 5.0f / 9.0f);
    lv_slider_set_left_value(temp_slider, displayLow, LV_ANIM_OFF);
    lv_slider_set_value(temp_slider, displayHigh, LV_ANIM_OFF);
    snprintf(buf_txt, sizeof(buf_txt), "Temperature: %.0f - %.0f \xb0%s", displayLow, displayHigh, useFahrenheit ? "F" : "C");
    if(temp_slider_label) lv_label_set_text(temp_slider_label, buf_txt);
  }
  if(hum_slider) {
    lv_slider_set_left_value(hum_slider, humLow, LV_ANIM_OFF);
    lv_slider_set_value(hum_slider, humHigh, LV_ANIM_OFF);
    snprintf(buf_txt, sizeof(buf_txt), "Humidity: %.0f - %.0f %%", humLow, humHigh);
    if(hum_slider_label) lv_label_set_text(hum_slider_label, buf_txt);
  }
  if(soil_slider) {
    lv_slider_set_left_value(soil_slider, soilLow, LV_ANIM_OFF);
    lv_slider_set_value(soil_slider, soilHigh, LV_ANIM_OFF);
    snprintf(buf_txt, sizeof(buf_txt), "Soil Moisture: %.0f - %.0f %%", soilLow, soilHigh);
    if(soil_slider_label) lv_label_set_text(soil_slider_label, buf_txt);
  }

  if(timer_sw) {
    if(timerEnabled) lv_obj_add_state(timer_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(timer_sw, LV_STATE_CHECKED);
  } 
  if(dm_sw) {
    if(isDarkMode) lv_obj_add_state(dm_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(dm_sw, LV_STATE_CHECKED);
  }
}

static bool parseTaggedFloat(const String &data, const char *tag, float &outValue) {
  int start = data.indexOf(tag);
  if(start < 0) return false;
  start += strlen(tag);
  int end = data.indexOf(',', start);
  if(end < 0) end = data.length();
  outValue = data.substring(start, end).toFloat();
  return true;
}

void updateClock() {
  const bool firstTick = (lastMinuteTick == 0);
  if(!firstTick && millis() - lastMinuteTick < 60000UL) return;

  lastMinuteTick = millis();

  if(WiFi.status() == WL_CONNECTED) {
    struct tm timeinfo;
    if(getLocalTime(&timeinfo, 10)) {
      currentHour = timeinfo.tm_hour;
      currentMinute = timeinfo.tm_min;
      return;
    }
  }

  if(firstTick) return;

  currentMinute++;
  if(currentMinute > 59) {
    currentMinute = 0;
    currentHour = (currentHour + 1) % 24;
  }
}

void evaluateControlLogic() {
  if(lastSerialRecv == 0 || millis() - lastSerialRecv > 15000UL) return;
  if(millis() - lastControlCheck < 5000UL) return;

  lastControlCheck = millis();

  const char *peltierCmd = "CMD:PELTIER_OFF";
  if(liveTemp > tempHigh) peltierCmd = "CMD:COOL";
  else if(liveTemp < tempLow) peltierCmd = "CMD:HEAT";

  if(lastPeltierCommand != String(peltierCmd)) {
    lastPeltierCommand = String(peltierCmd);
    sysLog("Arduino control -> " + lastPeltierCommand);
  }
  Serial2.println(peltierCmd);

  bool turnLightOn = (liveLux < luxThreshold);
  if(timerEnabled) {
    int nowMins = (currentHour * 60) + currentMinute;
    int onMins = (timeOnHour * 60) + timeOnMinute;
    int offMins = (timeOffHour * 60) + timeOffMinute;

    bool insideSchedule = false;
    if(onMins < offMins) insideSchedule = (nowMins >= onMins && nowMins < offMins);
    else insideSchedule = (nowMins >= onMins || nowMins < offMins);

    if(!insideSchedule) turnLightOn = false;
  }

  int pwmVal = turnLightOn ? map(globalBrightness, 1, 10, 25, 255) : 0;
  if(pwmVal != lastLightPwm) {
    lastLightPwm = pwmVal;
    sysLog("Arduino light PWM -> " + String(pwmVal));
  }
  Serial2.print("CMD:LIGHT,");
  Serial2.println(pwmVal);
}

void checkSerialSensors() {
  while(Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if(data.length() == 0) continue;

    Serial.print("[UART RX] ");
    Serial.println(data);

    bool changed = false;
    float value = 0.0f;

    if(parseTaggedFloat(data, "T:", value)) {
      liveTemp = useFahrenheit ? value : ((value - 32.0f) * 5.0f / 9.0f);
      Serial.print("  -> liveTemp = "); Serial.println(liveTemp);
      changed = true;
    }
    if(parseTaggedFloat(data, "H:", value)) {
      liveHum = value;
      Serial.print("  -> liveHum = "); Serial.println(liveHum);
      changed = true;
    }
    if(parseTaggedFloat(data, "L:", value)) {
      liveLux = value;
      Serial.print("  -> liveLux = "); Serial.println(liveLux);
      changed = true;
    }
    if(parseTaggedFloat(data, "S:", value) || parseTaggedFloat(data, "M:", value)) {
      liveSoil = value;
      Serial.print("  -> liveSoil = "); Serial.println(liveSoil);
      changed = true;
    }

    if(changed) {
      lastSerialRecv = millis();
      uiNeedsUpdate = true;
    } else {
      Serial.println("  -> No tags matched!");
    }
  }
}

void uiLoopTask(void * parameter) {
  for(;;) {
    lv_timer_handler();
    if(uiNeedsUpdate) update_ui_from_data();
    
    static unsigned long lastUpdate = 0;
    if(millis() - lastUpdate > 1000) {
        lastUpdate = millis();
        uiNeedsUpdate = true;
    }
    vTaskDelay(5 / portTICK_PERIOD_MS); 
  }
}

// ==========================================
// SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  Serial2.setTimeout(25);

// 1. INIT SCREEN & TOUCH
  tft.begin();
  tft.setRotation(1); // Landscape
  tft.setSwapBytes(true); 
  
  // Apply your unique touch settings
  uint16_t calData[5] = { 260, 3657, 273, 3580, 7 };
  tft.setTouch(calData);
  
  // 2. THE RED SCREEN TEST
  tft.fillScreen(TFT_RED); 
  delay(1000); 
  tft.fillScreen(TFT_BLACK);

  // --- Initialize LVGL v8 ---
  lv_init();

  // PSRAM ALLOCATION (Fixes DRAM Overflow Error)
  buf = (lv_color_t *)ps_malloc(screenWidth * 40 * sizeof(lv_color_t));
  if (buf == NULL) {
      Serial.println("PSRAM Allocation Failed!");
      while(1) yield();
  }

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // --- INITIALIZE SQUARELINE UI ---
  ui_init();
  fix_layout_for_display();

  // --- MAP SQUARELINE OBJECTS ---
  tabview = ui_TabView1;

  // These are mapped by their on-screen row/labels rather than the autogenerated object names.
  soil_slider = ui_temp_slider;
  temp_slider = ui_hum_slider;
  hum_slider = ui_soil_slider;
  lux_slider = ui_lux_slider;

  soil_slider_label = ui_MoistureLabel1;
  temp_slider_label = ui_TempLabel;
  hum_slider_label = ui_HumidityLabel;
  lux_slider_label = ui_LuxLabel;

  if(temp_slider) lv_obj_add_event_cb(temp_slider, temp_range_slider_cb, LV_EVENT_ALL, NULL);
  if(hum_slider) lv_obj_add_event_cb(hum_slider, hum_range_slider_cb, LV_EVENT_ALL, NULL);
  if(soil_slider) lv_obj_add_event_cb(soil_slider, soil_range_slider_cb, LV_EVENT_ALL, NULL);

  uiNeedsUpdate = true;
  update_ui_from_data();

  build_time_picker_modal();

  xTaskCreatePinnedToCore(uiLoopTask, "UITask", 16384, NULL, 2, &UILoopTaskHandle, 1);

  // --- WIFI BOOT ROUTINE ---
  isConnecting = true;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  load_wifi_credentials();

  bool wifiConnected = false;
  if(saved_ssid[0] != '\0') {
    wifiConnected = connect_to_wifi(saved_ssid, saved_pass, true);
  }

  if(!wifiConnected) {
    for(size_t i = 0; i < sizeof(myNetworks) / sizeof(myNetworks[0]); ++i) {
      if(connect_to_wifi(myNetworks[i].ssid, myNetworks[i].pass, false)) {
        save_wifi_credentials(myNetworks[i].ssid, myNetworks[i].pass);
        rtc_ssid_index = (int)i;
        wifiConnected = true;
        break;
      }
    }
  }

  if(wifiConnected) {
    load_settings_from_firebase();
  }

  if(CloudTaskHandle == NULL) {
    xTaskCreatePinnedToCore(cloudTask, "CloudTask", 12288, NULL, 1, &CloudTaskHandle, 0);
  }

  isConnecting = false;
  uiNeedsUpdate = true;
}

void loop() {
  checkSerialSensors();
  updateClock();
  evaluateControlLogic();
  delay(10); 
}