#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <esp_timer.h>
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
lv_obj_t * pwd_modal = NULL;
lv_obj_t * pwd_ta;
lv_obj_t * kb;
lv_obj_t * conn_status_label;
lv_obj_t * wifi_status_detail_label = NULL;
lv_obj_t * wifi_kb_corner_hide_btn = NULL;
char selected_ssid[64];
char saved_ssid[64] = {0};
char saved_pass[64] = {0};
lv_obj_t * wifi_kb_toggle_btn = NULL;
lv_obj_t * wifi_kb_toggle_label = NULL;
lv_obj_t * wifi_pwd_eye_btn = NULL;
lv_obj_t * wifi_pwd_eye_label = NULL;
lv_obj_t * wifi_connect_btn = NULL;
bool wifi_keyboard_hidden = false;
Preferences wifiPrefs;

static lv_obj_t * network_settings_page = NULL;
static lv_obj_t * network_connect_btn = NULL;
static lv_obj_t * network_forget_btn = NULL;
static lv_obj_t * dashboard_ph_value_label = NULL;
static lv_obj_t * dashboard_schedule_label = NULL;
static lv_obj_t * dashboard_schedule_state_label = NULL;
static lv_obj_t * dashboard_water_label = NULL;
static lv_obj_t * lighting_timer_card = NULL;
static lv_obj_t * lighting_timer_title = NULL;
static lv_obj_t * lighting_timer_state_label = NULL;
static lv_obj_t * lighting_set_on_btn = NULL;
static lv_obj_t * lighting_set_off_btn = NULL;
static lv_obj_t * watering_timer_card = NULL;
static lv_obj_t * watering_timer_state_label = NULL;
static lv_obj_t * water_timer_sw = NULL;
static lv_obj_t * water_timer_reset_btn = NULL;
static lv_obj_t * water_timer_set_btn = NULL;
static lv_obj_t * water_timer_set_label = NULL;

// --- TIME PICKER UI VARIABLES ---
lv_obj_t * time_picker_bg = NULL;
lv_obj_t * roller_h;
lv_obj_t * roller_m;
lv_obj_t * roller_ampm;
bool isEditingOnTime = true;
lv_obj_t * lbl_time_on;
lv_obj_t * lbl_time_off;
lv_obj_t * time_picker_title = NULL;
lv_obj_t * water_picker_bg = NULL;
lv_obj_t * water_picker_title = NULL;
lv_obj_t * water_picker_value_roller = NULL;
lv_obj_t * water_picker_unit_roller = NULL;

void build_wifi_scanner_ui(); 
void open_time_picker(bool isOnTime);
void build_water_timer_modal();
void open_water_timer_picker();
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
static void fix_wifi_list_layout();
static void fix_wifi_password_layout();
static void set_wifi_keyboard_visibility(bool hidden);
static void reset_menu_view();
static void update_network_status_label();
static void create_network_settings_page();
static void configure_dashboard_ph_widgets();
static void fix_network_settings_layout();
static void show_network_settings_page();
static void open_menu_root();
static bool wifi_overlay_active();
static void update_timer_labels();
static void ensure_lighting_timer_controls();
static void ensure_watering_timer_controls();
static void schedule_next_watering(bool resetFromNow);
static void update_watering_timer_label();
static void send_watering_command();

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
float liveTemp = 0.0, liveHum = 0.0, liveSoil = 0.0, liveLux = 0.0, livePh = -1.0;
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
bool waterTimerEnabled = false;
int waterIntervalValue = 1;
int waterIntervalUnit = 1;
time_t nextWateringEpoch = 0;
uint64_t nextWateringMillis = 0;

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
const char * water_value_str = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30";
const char * water_unit_str = "Hours\nDays\nWeeks";

static void format_time_12h(int hour24, int minute, char *buffer, size_t bufferSize) {
  int hour12 = hour24 % 12;
  if(hour12 == 0) hour12 = 12;
  snprintf(buffer, bufferSize, "%d:%02d %s", hour12, minute, hour24 >= 12 ? "PM" : "AM");
}

static uint64_t water_interval_ms() {
  uint64_t baseHours = (uint64_t)waterIntervalValue;
  if(waterIntervalUnit == 1) baseHours *= 24UL;
  else if(waterIntervalUnit == 2) baseHours *= (24UL * 7UL);
  return baseHours * 3600000ULL;
}

static void update_watering_timer_label() {
  char summary[96];
  const char *unit = waterIntervalUnit == 0 ? "Hour" : (waterIntervalUnit == 1 ? "Day" : "Week");
  snprintf(summary, sizeof(summary), "Water every %d %s%s", waterIntervalValue, unit, waterIntervalValue == 1 ? "" : "s");

  if(watering_timer_state_label != NULL) {
    lv_label_set_text_fmt(watering_timer_state_label, "%s\n%s", summary, waterTimerEnabled ? "Timer Enabled" : "Timer Disabled");
  }

  if(water_timer_set_label != NULL) {
    lv_label_set_text(water_timer_set_label, summary);
  }

  if(dashboard_water_label != NULL) {
    char dueBuf[48];
    if(waterTimerEnabled) {
      if(nextWateringEpoch > 100000) {
        struct tm nextInfo;
        localtime_r(&nextWateringEpoch, &nextInfo);
        char timeBuf[24];
        format_time_12h(nextInfo.tm_hour, nextInfo.tm_min, timeBuf, sizeof(timeBuf));
        snprintf(dueBuf, sizeof(dueBuf), "Next water: %s", timeBuf);
      } else {
        uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if(nextWateringMillis > nowMs) {
          uint64_t minsLeft = (nextWateringMillis - nowMs) / 60000ULL;
          snprintf(dueBuf, sizeof(dueBuf), "Next water in %llu min", (unsigned long long)minsLeft);
        } else {
          snprintf(dueBuf, sizeof(dueBuf), "Waiting for schedule");
        }
      }
      lv_label_set_text_fmt(dashboard_water_label, "%s\n%s", summary, dueBuf);
    } else {
      lv_label_set_text_fmt(dashboard_water_label, "%s\nTimer Disabled", summary);
    }
  }
}

static void update_timer_labels() {
  char onBuf[24];
  char offBuf[24];
  char scheduleBuf[64];

  format_time_12h(timeOnHour, timeOnMinute, onBuf, sizeof(onBuf));
  format_time_12h(timeOffHour, timeOffMinute, offBuf, sizeof(offBuf));
  snprintf(scheduleBuf, sizeof(scheduleBuf), "%s to %s", onBuf, offBuf);

  if(lbl_time_on != NULL) lv_label_set_text_fmt(lbl_time_on, "Start: %s", onBuf);
  if(lbl_time_off != NULL) lv_label_set_text_fmt(lbl_time_off, "Stop: %s", offBuf);
  if(lighting_timer_state_label != NULL) lv_label_set_text_fmt(lighting_timer_state_label, "%s\n%s", scheduleBuf, timerEnabled ? "Timer Enabled" : "Timer Disabled");
  if(dashboard_schedule_label != NULL) lv_label_set_text(dashboard_schedule_label, scheduleBuf);
  if(dashboard_schedule_state_label != NULL) lv_label_set_text(dashboard_schedule_state_label, timerEnabled ? "Timer Enabled" : "Timer Disabled");

  update_watering_timer_label();
}

static bool wifi_overlay_active() {
  return wifi_list != NULL || pwd_screen != NULL || isScanning || isConnecting;
}

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
    pwd_modal = NULL;
    pwd_ta = NULL;
    kb = NULL;
    conn_status_label = NULL;
    wifi_kb_corner_hide_btn = NULL;
    wifi_kb_toggle_btn = NULL;
    wifi_kb_toggle_label = NULL;
    wifi_connect_btn = NULL;
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

static void set_wifi_keyboard_visibility(bool hidden) {
  wifi_keyboard_hidden = hidden;

  if(kb != NULL) {
    if(hidden) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }

  if(wifi_kb_corner_hide_btn != NULL) {
    if(hidden) lv_obj_add_flag(wifi_kb_corner_hide_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(wifi_kb_corner_hide_btn, LV_OBJ_FLAG_HIDDEN);
  }

  if(wifi_kb_toggle_label != NULL) {
    lv_label_set_text(wifi_kb_toggle_label, hidden ? "Show KB" : "Hide KB");
  }

  if(!hidden && kb != NULL && pwd_ta != NULL) {
    _ui_keyboard_set_target(kb, pwd_ta);
  }
}

static void reset_menu_view() {
  if(ui_env_page != NULL) lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_HIDDEN);
  if(ui_lighting_page != NULL) lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_HIDDEN);
  if(ui_display_page != NULL) lv_obj_add_flag(ui_display_page, LV_OBJ_FLAG_HIDDEN);
  if(network_settings_page != NULL) lv_obj_add_flag(network_settings_page, LV_OBJ_FLAG_HIDDEN);
  if(ui_menu_list != NULL) lv_obj_clear_flag(ui_menu_list, LV_OBJ_FLAG_HIDDEN);
  if(ui_btn_back_global != NULL) lv_obj_add_flag(ui_btn_back_global, LV_OBJ_FLAG_HIDDEN);
}

static void open_menu_root() {
  close_wifi_overlays(true);
  reset_menu_view();
  if(tabview != NULL) lv_tabview_set_act(tabview, 1, LV_ANIM_OFF);
}

static void show_network_settings_page() {
  close_wifi_overlays(true);
  if(tabview != NULL) {
    lv_tabview_set_act(tabview, 1, LV_ANIM_OFF);
    lv_obj_clear_flag(tabview, LV_OBJ_FLAG_HIDDEN);
  }
  reset_menu_view();
  if(network_settings_page != NULL) lv_obj_clear_flag(network_settings_page, LV_OBJ_FLAG_HIDDEN);
  if(ui_menu_list != NULL) lv_obj_add_flag(ui_menu_list, LV_OBJ_FLAG_HIDDEN);
  if(ui_btn_back_global != NULL) lv_obj_clear_flag(ui_btn_back_global, LV_OBJ_FLAG_HIDDEN);
  update_network_status_label();
}

static void update_network_status_label() {
  if(wifi_status_detail_label == NULL) return;

  if(WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(wifi_status_detail_label, "Connected: %s\nSaved: %s",
                          WiFi.SSID().c_str(),
                          saved_ssid[0] != '\0' ? saved_ssid : "None");
    return;
  }

  if(saved_ssid[0] != '\0') lv_label_set_text_fmt(wifi_status_detail_label, "Not connected\nSaved: %s", saved_ssid);
  else lv_label_set_text(wifi_status_detail_label, "Not connected\nSaved: None");
}

static void fix_network_settings_layout() {
  if(network_settings_page == NULL) return;

  lv_obj_set_size(network_settings_page, lv_pct(100), lv_pct(100));
  lv_obj_align(network_settings_page, LV_ALIGN_CENTER, 0, 0);
}

static void fix_wifi_list_layout() {
  if(wifi_list == NULL) return;

  lv_obj_set_size(wifi_list, screenWidth - 24, screenHeight - 24);
  lv_obj_center(wifi_list);
}

static void fix_wifi_password_layout() {
  if(pwd_screen == NULL) return;

  lv_obj_set_size(pwd_screen, screenWidth, screenHeight);
  lv_obj_center(pwd_screen);

  if(pwd_modal != NULL) {
    const lv_coord_t modal_width = screenWidth - 28;
    const lv_coord_t modal_height = screenHeight - 18;

    lv_obj_set_size(pwd_modal, modal_width, modal_height);
    lv_obj_center(pwd_modal);

    if(pwd_ta != NULL) {
      lv_obj_set_size(pwd_ta, modal_width - 82, 40);
      lv_obj_align(pwd_ta, LV_ALIGN_TOP_LEFT, 0, 86);
    }

    if(wifi_pwd_eye_btn != NULL && pwd_ta != NULL) {
      lv_obj_set_size(wifi_pwd_eye_btn, 46, 40);
      lv_obj_align_to(wifi_pwd_eye_btn, pwd_ta, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }

    if(conn_status_label != NULL) {
      lv_obj_set_width(conn_status_label, modal_width - 52);
      lv_obj_align(conn_status_label, LV_ALIGN_TOP_LEFT, 0, 136);
    }

    if(wifi_kb_toggle_btn != NULL) {
      lv_obj_set_size(wifi_kb_toggle_btn, 96, 34);
      lv_obj_align(wifi_kb_toggle_btn, LV_ALIGN_TOP_RIGHT, 0, 174);
    }

    if(wifi_connect_btn != NULL) {
      lv_obj_set_size(wifi_connect_btn, 46, 46);
      lv_obj_align_to(wifi_connect_btn, pwd_screen, LV_ALIGN_TOP_RIGHT, -10, 10);
      lv_obj_set_style_radius(wifi_connect_btn, LV_RADIUS_CIRCLE, 0);
    }
  }

  if(kb != NULL) {
    lv_obj_set_size(kb, screenWidth - 54, 122);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  }

  if(wifi_kb_corner_hide_btn != NULL && kb != NULL) {
    lv_obj_set_size(wifi_kb_corner_hide_btn, 44, 28);
    lv_obj_align_to(wifi_kb_corner_hide_btn, kb, LV_ALIGN_BOTTOM_LEFT, 6, -6);
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

  if(WiFi.status() == WL_CONNECTED && WiFi.SSID().equals(ssid) && WiFi.localIP()[0] != 0) {
    update_network_status_label();
    return true;
  }

  isConnecting = true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if(conn_status_label) lv_label_set_text_fmt(conn_status_label, "Connecting to %s...", ssid);
  sysLog("Connecting to " + String(ssid));

  WiFi.disconnect(true, true);
  delay(250);

  if(useFastReconnect && rtc_bssid_valid) {
    WiFi.begin(ssid, password, rtc_channel, rtc_bssid, true);
  } else {
    WiFi.begin(ssid, password);
  }

  unsigned long start = millis();
  wl_status_t status = WL_IDLE_STATUS;
  while(millis() - start < 15000) {
    status = WiFi.status();
    if(status == WL_CONNECTED && WiFi.localIP()[0] != 0) break;
    if(status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
    delay(250);
  }

  isConnecting = false;

  status = WiFi.status();
  if(status == WL_CONNECTED && WiFi.localIP()[0] != 0) {
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
    update_network_status_label();
    uiNeedsUpdate = true;
    return true;
  }

  rtc_bssid_valid = 0;
  sysLog("WiFi connection failed for " + String(ssid));
  WiFi.disconnect(true, true);
  if(conn_status_label) {
    if(status == WL_NO_SSID_AVAIL) lv_label_set_text(conn_status_label, "Network not found.");
    else lv_label_set_text(conn_status_label, "Connection failed. Check the password.");
  }
  update_network_status_label();
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
  doc["waterTimerEnabled"] = waterTimerEnabled;
  doc["waterIntervalValue"] = waterIntervalValue;
  doc["waterIntervalUnit"] = waterIntervalUnit;

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
  if(livePh >= 0.0f) {
    doc["livePh"] = livePh;
    doc["currentPh"] = livePh;
  }

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
  if(!doc["waterTimerEnabled"].isNull() && waterTimerEnabled != doc["waterTimerEnabled"].as<bool>()) {
    waterTimerEnabled = doc["waterTimerEnabled"].as<bool>();
    schedule_next_watering(true);
    changed = true;
  }
  if(!doc["waterIntervalValue"].isNull() && waterIntervalValue != doc["waterIntervalValue"].as<int>()) {
    waterIntervalValue = constrain(doc["waterIntervalValue"].as<int>(), 1, 30);
    schedule_next_watering(true);
    changed = true;
  }
  if(!doc["waterIntervalUnit"].isNull() && waterIntervalUnit != doc["waterIntervalUnit"].as<int>()) {
    waterIntervalUnit = constrain(doc["waterIntervalUnit"].as<int>(), 0, 2);
    schedule_next_watering(true);
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

  if(!doc["luxThreshold"].isNull() && fabsf(doc["luxThreshold"].as<float>() - luxThreshold) > 0.5f) { luxThreshold = constrain(doc["luxThreshold"].as<float>(), 0.0f, 40000.0f); changed = true; }
  else if(!doc["lighting"]["luxThreshold"].isNull() && fabsf(doc["lighting"]["luxThreshold"].as<float>() - luxThreshold) > 0.5f) { luxThreshold = constrain(doc["lighting"]["luxThreshold"].as<float>(), 0.0f, 40000.0f); changed = true; }

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
    if(wifi_overlay_active()) {
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
    lv_obj_set_style_pad_left(ui_status_bar, 8, 0);
    lv_obj_set_style_pad_right(ui_status_bar, 8, 0);
  }

  if(ui_wifistatuslabel) {
    lv_obj_align(ui_wifistatuslabel, LV_ALIGN_RIGHT_MID, -8, 0);
  }

  if(ui_TabView1) {
    lv_obj_set_size(ui_TabView1, screenWidth, screenHeight - 30);
    lv_obj_align(ui_TabView1, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t * tab_content = lv_tabview_get_content(ui_TabView1);
    if(tab_content != NULL) {
      lv_obj_clear_flag(tab_content, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scrollbar_mode(tab_content, LV_SCROLLBAR_MODE_OFF);
    }
  }

  if(ui_Dashboard) {
    lv_obj_add_flag(ui_Dashboard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_Dashboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_Dashboard, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(ui_Dashboard, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_Dashboard, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(ui_Dashboard, 36, 0);
  }

  if(ui_env_page) {
    lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(ui_env_page, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(ui_env_page, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(ui_env_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_env_page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_top(ui_env_page, 96, 0);
    lv_obj_set_style_pad_bottom(ui_env_page, 80, 0);
  }

  if(ui_lighting_page) {
    lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(ui_lighting_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_lighting_page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(ui_lighting_page, 36, 0);
  }

  fix_network_settings_layout();
  configure_dashboard_ph_widgets();
  ensure_lighting_timer_controls();
  ensure_watering_timer_controls();
  update_timer_labels();
  fix_wifi_list_layout();
  fix_wifi_password_layout();
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
    reset_menu_view();
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

void open_wifi_scanner_cb(lv_event_t * e) {
  LV_UNUSED(e);
  sysLog("Opening network settings...");
  show_network_settings_page();
}

static void tabview_tab_buttons_cb(lv_event_t * e) {
  if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

  lv_obj_t * btns = lv_event_get_target(e);
  if(btns == NULL) return;

  uint16_t selected = lv_btnmatrix_get_selected_btn(btns);
  if(selected == 1) {
    open_menu_root();
  }
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

void lux_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    luxThreshold = constrain((float)lv_slider_get_value(slider), 0.0f, 40000.0f);
    if(lux_slider_label) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d lux", (int)lroundf(luxThreshold));
      lv_label_set_text(lux_slider_label, buf);
    }
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true;
}

static void timer_switch_cb(lv_event_t * e) {
  timerEnabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  update_timer_labels();
  triggerCloudPush = true;
}

static void water_timer_switch_cb(lv_event_t * e) {
  waterTimerEnabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  schedule_next_watering(true);
  update_watering_timer_label();
  triggerCloudPush = true;
}

static void water_timer_set_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  open_water_timer_picker();
}

static void water_timer_reset_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  schedule_next_watering(true);
  update_watering_timer_label();
  uiNeedsUpdate = true;
}

static void lighting_set_on_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  open_time_picker(true);
}

static void lighting_set_off_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  open_time_picker(false);
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
    open_menu_root();
  }
}

static void wifi_keyboard_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);

  if(code == LV_EVENT_READY) {
    wifi_connect_now();
  } else if(code == LV_EVENT_CANCEL) {
    set_wifi_keyboard_visibility(true);
  }
}

static void wifi_textarea_event_cb(lv_event_t * e) {
  if(lv_event_get_code(e) == LV_EVENT_CLICKED && kb != NULL) {
    set_wifi_keyboard_visibility(false);
  }
}

static void wifi_cancel_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  close_wifi_overlays(false);

  if(wifi_list != NULL) {
    lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
  } else {
    show_network_settings_page();
  }
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

  set_wifi_keyboard_visibility(!wifi_keyboard_hidden);
}

static void wifi_keyboard_corner_hide_cb(lv_event_t * e) {
  LV_UNUSED(e);
  set_wifi_keyboard_visibility(true);
}

static void network_connect_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  if(network_settings_page != NULL) lv_obj_add_flag(network_settings_page, LV_OBJ_FLAG_HIDDEN);
  build_wifi_scanner_ui();
}

static void forget_wifi_credentials() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.clear();
  wifiPrefs.end();

  saved_ssid[0] = '\0';
  saved_pass[0] = '\0';
  selected_ssid[0] = '\0';
  rtc_bssid_valid = 0;
  rtc_ssid_index = -1;
  WiFi.disconnect(true, true);
  sysLog("Saved WiFi credentials cleared.");
  update_network_status_label();
  uiNeedsUpdate = true;
}

static void network_forget_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  forget_wifi_credentials();
}

static void tabview_changed_cb(lv_event_t * e) {
  if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

  lv_obj_t * tv = lv_event_get_target(e);
  if(lv_tabview_get_tab_act(tv) == 1) {
    reset_menu_view();
  }
}

static void create_network_settings_page() {
  if(ui_Menu == NULL || network_settings_page != NULL) return;

  network_settings_page = lv_obj_create(ui_Menu);
  lv_obj_remove_style_all(network_settings_page);
  lv_obj_add_flag(network_settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(network_settings_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(network_settings_page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(network_settings_page, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_top(network_settings_page, 16, 0);
  lv_obj_set_style_pad_bottom(network_settings_page, 40, 0);

  lv_obj_t * title = lv_label_create(network_settings_page);
  lv_label_set_text(title, "Network Settings");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  wifi_status_detail_label = lv_label_create(network_settings_page);
  lv_obj_set_width(wifi_status_detail_label, 300);
  lv_label_set_long_mode(wifi_status_detail_label, LV_LABEL_LONG_WRAP);
  lv_obj_align(wifi_status_detail_label, LV_ALIGN_TOP_MID, 0, 56);

  network_connect_btn = lv_btn_create(network_settings_page);
  lv_obj_set_size(network_connect_btn, 260, 48);
  lv_obj_align(network_connect_btn, LV_ALIGN_TOP_MID, 0, 138);
  lv_obj_add_event_cb(network_connect_btn, network_connect_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * connect_label = lv_label_create(network_connect_btn);
  lv_label_set_text(connect_label, "Network Connect");
  lv_obj_center(connect_label);

  network_forget_btn = lv_btn_create(network_settings_page);
  lv_obj_set_size(network_forget_btn, 260, 48);
  lv_obj_align(network_forget_btn, LV_ALIGN_TOP_MID, 0, 206);
  lv_obj_add_event_cb(network_forget_btn, network_forget_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * forget_label = lv_label_create(network_forget_btn);
  lv_label_set_text(forget_label, "Forget WiFi");
  lv_obj_center(forget_label);

  update_network_status_label();
  fix_network_settings_layout();
}

static void configure_dashboard_ph_widgets() {
  if(ui_Dashboard == NULL || ui_pHlabel == NULL || ui_Bar1 == NULL) return;

  if(lv_obj_get_parent(ui_pHlabel) != ui_Dashboard) lv_obj_set_parent(ui_pHlabel, ui_Dashboard);
  if(lv_obj_get_parent(ui_Bar1) != ui_Dashboard) lv_obj_set_parent(ui_Bar1, ui_Dashboard);
  if(ui_Image2 != NULL) lv_obj_add_flag(ui_Image2, LV_OBJ_FLAG_HIDDEN);

  lv_obj_set_x(ui_pHlabel, 12);
  lv_obj_set_y(ui_pHlabel, 164);
  lv_obj_set_align(ui_pHlabel, LV_ALIGN_TOP_MID);

  lv_obj_set_width(ui_Bar1, 170);
  lv_obj_set_height(ui_Bar1, 12);
  lv_obj_set_x(ui_Bar1, 0);
  lv_obj_set_y(ui_Bar1, 196);
  lv_obj_set_align(ui_Bar1, LV_ALIGN_TOP_MID);

  if(dashboard_schedule_label == NULL) {
    dashboard_schedule_label = lv_label_create(ui_Dashboard);
    lv_obj_set_width(dashboard_schedule_label, 280);
    lv_label_set_long_mode(dashboard_schedule_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(dashboard_schedule_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(dashboard_schedule_label, LV_ALIGN_TOP_MID, 0, 228);
  }

  if(dashboard_schedule_state_label == NULL) {
    dashboard_schedule_state_label = lv_label_create(ui_Dashboard);
    lv_obj_set_width(dashboard_schedule_state_label, 280);
    lv_label_set_long_mode(dashboard_schedule_state_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(dashboard_schedule_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(dashboard_schedule_state_label, LV_ALIGN_TOP_MID, 0, 256);
  }

  if(dashboard_water_label == NULL) {
    dashboard_water_label = lv_label_create(ui_Dashboard);
    lv_obj_set_width(dashboard_water_label, 280);
    lv_label_set_long_mode(dashboard_water_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(dashboard_water_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(dashboard_water_label, LV_ALIGN_TOP_MID, 0, 292);
  }

  dashboard_ph_value_label = ui_pHlabel;
}

static void ensure_lighting_timer_controls() {
  if(ui_lighting_page == NULL || lighting_timer_card != NULL) return;

  if(ui_Image1 != NULL) {
    lv_obj_align(ui_Image1, LV_ALIGN_TOP_MID, 0, 12);
    lv_img_set_zoom(ui_Image1, 220);
  }
  if(ui_LuxLabel != NULL) {
    lv_obj_align(ui_LuxLabel, LV_ALIGN_TOP_MID, 0, 56);
  }
  if(ui_lux_slider != NULL) {
    lv_obj_set_width(ui_lux_slider, 260);
    lv_obj_align(ui_lux_slider, LV_ALIGN_TOP_MID, 0, 92);
  }

  lighting_timer_card = lv_obj_create(ui_lighting_page);
  lv_obj_set_size(lighting_timer_card, 340, 150);
  lv_obj_align(lighting_timer_card, LV_ALIGN_TOP_MID, 0, 132);
  lv_obj_set_style_pad_all(lighting_timer_card, 12, 0);

  lighting_timer_title = lv_label_create(lighting_timer_card);
  lv_label_set_text(lighting_timer_title, "Lighting Timer");
  lv_obj_align(lighting_timer_title, LV_ALIGN_TOP_LEFT, 0, 0);

  if(timer_sw == NULL) {
    timer_sw = lv_switch_create(lighting_timer_card);
    lv_obj_align(timer_sw, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(timer_sw, timer_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
  }

  lbl_time_on = lv_label_create(lighting_timer_card);
  lv_obj_align(lbl_time_on, LV_ALIGN_TOP_LEFT, 0, 34);

  lbl_time_off = lv_label_create(lighting_timer_card);
  lv_obj_align(lbl_time_off, LV_ALIGN_TOP_LEFT, 0, 60);

  lighting_set_on_btn = lv_btn_create(lighting_timer_card);
  lv_obj_set_size(lighting_set_on_btn, 132, 36);
  lv_obj_align(lighting_set_on_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(lighting_set_on_btn, lighting_set_on_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * onBtnLabel = lv_label_create(lighting_set_on_btn);
  lv_label_set_text(onBtnLabel, "Set Start");
  lv_obj_center(onBtnLabel);

  lighting_set_off_btn = lv_btn_create(lighting_timer_card);
  lv_obj_set_size(lighting_set_off_btn, 132, 36);
  lv_obj_align(lighting_set_off_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(lighting_set_off_btn, lighting_set_off_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * offBtnLabel = lv_label_create(lighting_set_off_btn);
  lv_label_set_text(offBtnLabel, "Set Stop");
  lv_obj_center(offBtnLabel);

  lighting_timer_state_label = lv_label_create(lighting_timer_card);
  lv_obj_set_width(lighting_timer_state_label, 170);
  lv_label_set_long_mode(lighting_timer_state_label, LV_LABEL_LONG_WRAP);
  lv_obj_align(lighting_timer_state_label, LV_ALIGN_TOP_RIGHT, 0, 34);
}

static void ensure_watering_timer_controls() {
  if(ui_env_page == NULL || watering_timer_card != NULL) return;

  watering_timer_card = lv_obj_create(ui_env_page);
  lv_obj_set_size(watering_timer_card, 380, 168);
  lv_obj_align(watering_timer_card, LV_ALIGN_TOP_MID, 0, 224);
  lv_obj_set_style_pad_all(watering_timer_card, 14, 0);
  lv_obj_add_flag(watering_timer_card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_flag(watering_timer_card, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t * title = lv_label_create(watering_timer_card);
  lv_label_set_text(title, "Water Pump Timer");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  water_timer_sw = lv_switch_create(watering_timer_card);
  lv_obj_align(water_timer_sw, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_flag(water_timer_sw, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_flag(water_timer_sw, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(water_timer_sw, water_timer_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

  water_timer_set_btn = lv_btn_create(watering_timer_card);
  lv_obj_set_size(water_timer_set_btn, 244, 40);
  lv_obj_align(water_timer_set_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_flag(water_timer_set_btn, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_flag(water_timer_set_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(water_timer_set_btn, water_timer_set_btn_cb, LV_EVENT_CLICKED, NULL);
  water_timer_set_label = lv_label_create(water_timer_set_btn);
  lv_label_set_text(water_timer_set_label, "Water every 1 Day");
  lv_obj_center(water_timer_set_label);

  watering_timer_state_label = lv_label_create(watering_timer_card);
  lv_obj_set_width(watering_timer_state_label, 240);
  lv_label_set_long_mode(watering_timer_state_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(watering_timer_state_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(watering_timer_state_label, LV_ALIGN_TOP_MID, 0, 44);

  water_timer_reset_btn = lv_btn_create(watering_timer_card);
  lv_obj_set_size(water_timer_reset_btn, 40, 40);
  lv_obj_align(water_timer_reset_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(water_timer_reset_btn, 12, 0);
  lv_obj_add_flag(water_timer_reset_btn, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_flag(water_timer_reset_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(water_timer_reset_btn, water_timer_reset_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * resetLabel = lv_label_create(water_timer_reset_btn);
  lv_label_set_text(resetLabel, LV_SYMBOL_REFRESH);
  lv_obj_center(resetLabel);
}

void build_wifi_password_ui(const char *ssid) {
  if(ssid == NULL || ssid[0] == '\0') return;

  snprintf(selected_ssid, sizeof(selected_ssid), "%s", ssid);
  close_wifi_overlays(false);

  pwd_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_style_bg_color(pwd_screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(pwd_screen, LV_OPA_70, 0);
  lv_obj_set_style_border_width(pwd_screen, 0, 0);
  lv_obj_clear_flag(pwd_screen, LV_OBJ_FLAG_SCROLLABLE);

  pwd_modal = lv_obj_create(pwd_screen);
  lv_obj_clear_flag(pwd_modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(pwd_modal, 8, 0);

  lv_obj_t * back_btn = lv_btn_create(pwd_modal);
  lv_obj_set_size(back_btn, 52, 32);
  lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t * back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, wifi_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t * title = lv_label_create(pwd_modal);
  lv_label_set_text(title, "Connect to WiFi");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t * ssid_label = lv_label_create(pwd_modal);
  lv_obj_set_width(ssid_label, screenWidth - 80);
  lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(ssid_label, "SSID: %s", ssid);
  lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 0, 42);

  pwd_ta = lv_textarea_create(pwd_modal);
  lv_textarea_set_placeholder_text(pwd_ta, "Enter WiFi password");
  lv_textarea_set_password_mode(pwd_ta, true);
  lv_textarea_set_one_line(pwd_ta, true);
  lv_obj_add_event_cb(pwd_ta, wifi_textarea_event_cb, LV_EVENT_CLICKED, NULL);

  if(strcmp(saved_ssid, ssid) == 0 && saved_pass[0] != '\0') {
    lv_textarea_set_text(pwd_ta, saved_pass);
  }

  wifi_pwd_eye_btn = lv_btn_create(pwd_modal);
  wifi_pwd_eye_label = lv_label_create(wifi_pwd_eye_btn);
  lv_label_set_text(wifi_pwd_eye_label, LV_SYMBOL_EYE_OPEN);
  lv_obj_center(wifi_pwd_eye_label);
  lv_obj_add_event_cb(wifi_pwd_eye_btn, wifi_toggle_password_eye_cb, LV_EVENT_CLICKED, NULL);

  conn_status_label = lv_label_create(pwd_modal);
  lv_label_set_long_mode(conn_status_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(conn_status_label, "Tap the password field to show the keyboard. Use the keyboard checkmark to connect.");

  wifi_kb_toggle_btn = lv_btn_create(pwd_modal);
  wifi_kb_toggle_label = lv_label_create(wifi_kb_toggle_btn);
  lv_label_set_text(wifi_kb_toggle_label, "Show KB");
  lv_obj_center(wifi_kb_toggle_label);
  lv_obj_add_event_cb(wifi_kb_toggle_btn, wifi_toggle_keyboard_btn_cb, LV_EVENT_CLICKED, NULL);

  wifi_connect_btn = lv_btn_create(pwd_screen);
  lv_obj_set_style_bg_color(wifi_connect_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_bg_opa(wifi_connect_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(wifi_connect_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(wifi_connect_btn, 0, 0);
  lv_obj_add_event_cb(wifi_connect_btn, [](lv_event_t * e){
    LV_UNUSED(e);
    wifi_connect_now();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * connect_btn_label = lv_label_create(wifi_connect_btn);
  lv_label_set_text(connect_btn_label, LV_SYMBOL_OK);
  lv_obj_set_style_text_color(connect_btn_label, lv_color_white(), 0);
  lv_obj_center(connect_btn_label);
  lv_obj_set_size(wifi_connect_btn, 46, 46);
  lv_obj_align(wifi_connect_btn, LV_ALIGN_TOP_RIGHT, -10, 10);

  wifi_kb_corner_hide_btn = lv_btn_create(pwd_modal);
  lv_obj_add_event_cb(wifi_kb_corner_hide_btn, wifi_keyboard_corner_hide_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t * corner_hide_label = lv_label_create(wifi_kb_corner_hide_btn);
  lv_label_set_text(corner_hide_label, LV_SYMBOL_KEYBOARD);
  lv_obj_center(corner_hide_label);

  kb = lv_keyboard_create(pwd_modal);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(kb, wifi_keyboard_event_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb, wifi_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
  _ui_keyboard_set_target(kb, pwd_ta);

  set_wifi_keyboard_visibility(true);

  fix_wifi_password_layout();
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
  fix_wifi_list_layout();

  lv_obj_t * back_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_LEFT, "Back to Settings");
  lv_obj_add_event_cb(back_btn, [](lv_event_t * e){
    LV_UNUSED(e);
    show_network_settings_page();
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
  lv_obj_set_style_border_width(time_picker_bg, 0, 0);
  lv_obj_clear_flag(time_picker_bg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t * modal = lv_obj_create(time_picker_bg);
  lv_obj_set_size(modal, 360, 220);
  lv_obj_center(modal);
  lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(modal, 12, 0);

  time_picker_title = lv_label_create(modal);
  lv_label_set_text(time_picker_title, "Select Time");
  lv_obj_align(time_picker_title, LV_ALIGN_TOP_LEFT, 0, 0);

  roller_h = lv_roller_create(modal);
  lv_roller_set_options(roller_h, hrs_str, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(roller_h, 3);
  lv_obj_set_size(roller_h, 84, 118);
  lv_obj_align(roller_h, LV_ALIGN_CENTER, -96, -6);

  roller_m = lv_roller_create(modal);
  lv_roller_set_options(roller_m, mins_str, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(roller_m, 3);
  lv_obj_set_size(roller_m, 84, 118);
  lv_obj_align(roller_m, LV_ALIGN_CENTER, 0, -6);

  roller_ampm = lv_roller_create(modal);
  lv_roller_set_options(roller_ampm, ampm_str, LV_ROLLER_MODE_INFINITE);
  lv_roller_set_visible_row_count(roller_ampm, 3);
  lv_obj_set_size(roller_ampm, 84, 92);
  lv_obj_align(roller_ampm, LV_ALIGN_CENTER, 96, 0);

  lv_obj_t * cancel_btn = lv_btn_create(modal);
  lv_obj_set_size(cancel_btn, 120, 36);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t * cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, "Cancel");
  lv_obj_center(cancel_lbl);
  lv_obj_add_event_cb(cancel_btn, [](lv_event_t * e){ LV_UNUSED(e); lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * save_btn = lv_btn_create(modal);
  lv_obj_set_size(save_btn, 120, 36);
  lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_t * save_lbl = lv_label_create(save_btn);
  lv_label_set_text(save_lbl, "Save");
  lv_obj_center(save_lbl);
  lv_obj_add_event_cb(save_btn, [](lv_event_t * e){
    LV_UNUSED(e);

    int hour12 = lv_roller_get_selected(roller_h) + 1;
    int minute = lv_roller_get_selected(roller_m);
    bool isPm = lv_roller_get_selected(roller_ampm) == 1;
    int hour24 = hour12 % 12;
    if(isPm) hour24 += 12;

    if(isEditingOnTime) {
      timeOnHour = hour24;
      timeOnMinute = minute;
    } else {
      timeOffHour = hour24;
      timeOffMinute = minute;
    }

    triggerCloudPush = true;
    update_timer_labels();
    lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);
}

void open_time_picker(bool isOnTime) {
  if(time_picker_bg == NULL || roller_h == NULL || roller_m == NULL || roller_ampm == NULL) return;

  isEditingOnTime = isOnTime;

  const int sourceHour = isOnTime ? timeOnHour : timeOffHour;
  const int sourceMinute = isOnTime ? timeOnMinute : timeOffMinute;
  int hour12 = sourceHour % 12;
  if(hour12 == 0) hour12 = 12;

  if(time_picker_title != NULL) lv_label_set_text(time_picker_title, isOnTime ? "Select Start Time" : "Select Stop Time");
  lv_roller_set_selected(roller_h, hour12 - 1, LV_ANIM_OFF);
  lv_roller_set_selected(roller_m, sourceMinute, LV_ANIM_OFF);
  lv_roller_set_selected(roller_ampm, sourceHour >= 12 ? 1 : 0, LV_ANIM_OFF);
  lv_obj_clear_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN);
}

void build_water_timer_modal() {
  water_picker_bg = lv_obj_create(lv_scr_act());
  lv_obj_set_size(water_picker_bg, screenWidth, screenHeight);
  lv_obj_center(water_picker_bg);
  lv_obj_set_style_bg_color(water_picker_bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(water_picker_bg, LV_OPA_80, 0);
  lv_obj_set_style_border_width(water_picker_bg, 0, 0);
  lv_obj_clear_flag(water_picker_bg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(water_picker_bg, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t * modal = lv_obj_create(water_picker_bg);
  lv_obj_set_size(modal, 360, 220);
  lv_obj_center(modal);
  lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(modal, 12, 0);

  water_picker_title = lv_label_create(modal);
  lv_label_set_text(water_picker_title, "Set Water Timer");
  lv_obj_align(water_picker_title, LV_ALIGN_TOP_LEFT, 0, 0);

  water_picker_value_roller = lv_roller_create(modal);
  lv_roller_set_options(water_picker_value_roller, water_value_str, LV_ROLLER_MODE_INFINITE);
  lv_roller_set_visible_row_count(water_picker_value_roller, 3);
  lv_obj_set_size(water_picker_value_roller, 96, 118);
  lv_obj_align(water_picker_value_roller, LV_ALIGN_CENTER, -74, -6);

  water_picker_unit_roller = lv_roller_create(modal);
  lv_roller_set_options(water_picker_unit_roller, water_unit_str, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(water_picker_unit_roller, 3);
  lv_obj_set_size(water_picker_unit_roller, 136, 118);
  lv_obj_align(water_picker_unit_roller, LV_ALIGN_CENTER, 74, -6);

  lv_obj_t * cancel_btn = lv_btn_create(modal);
  lv_obj_set_size(cancel_btn, 120, 36);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t * cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, "Cancel");
  lv_obj_center(cancel_lbl);
  lv_obj_add_event_cb(cancel_btn, [](lv_event_t * e){ LV_UNUSED(e); lv_obj_add_flag(water_picker_bg, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * save_btn = lv_btn_create(modal);
  lv_obj_set_size(save_btn, 120, 36);
  lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_t * save_lbl = lv_label_create(save_btn);
  lv_label_set_text(save_lbl, "Save");
  lv_obj_center(save_lbl);
  lv_obj_add_event_cb(save_btn, [](lv_event_t * e){
    LV_UNUSED(e);
    if(water_picker_value_roller == NULL || water_picker_unit_roller == NULL) return;

    waterIntervalValue = lv_roller_get_selected(water_picker_value_roller) + 1;
    waterIntervalUnit = lv_roller_get_selected(water_picker_unit_roller);
    schedule_next_watering(true);
    update_watering_timer_label();
    triggerCloudPush = true;
    lv_obj_add_flag(water_picker_bg, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);
}

void open_water_timer_picker() {
  if(water_picker_bg == NULL || water_picker_value_roller == NULL || water_picker_unit_roller == NULL) return;

  lv_roller_set_selected(water_picker_value_roller, waterIntervalValue - 1, LV_ANIM_OFF);
  lv_roller_set_selected(water_picker_unit_roller, waterIntervalUnit, LV_ANIM_OFF);
  lv_obj_clear_flag(water_picker_bg, LV_OBJ_FLAG_HIDDEN);
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
  if(dashboard_ph_value_label) {
    if(livePh >= 0.0f) snprintf(buf_txt, sizeof(buf_txt), "pH: %.2f", livePh);
    else snprintf(buf_txt, sizeof(buf_txt), "pH: --.--");
    lv_label_set_text(dashboard_ph_value_label, buf_txt);
  }
  if(ui_Bar1) {
    int ph_value = livePh >= 0.0f ? (int)lroundf(fminf(fmaxf(livePh, 0.0f), 14.0f)) : 0;
    lv_bar_set_value(ui_Bar1, ph_value, LV_ANIM_OFF);
  }
  update_network_status_label();

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
  if(lux_slider) {
    lv_slider_set_value(lux_slider, (int)lroundf(luxThreshold), LV_ANIM_OFF);
    if(lux_slider_label) {
      snprintf(buf_txt, sizeof(buf_txt), "%d lux", (int)lroundf(luxThreshold));
      lv_label_set_text(lux_slider_label, buf_txt);
    }
  }

  if(timer_sw) {
    if(timerEnabled) lv_obj_add_state(timer_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(timer_sw, LV_STATE_CHECKED);
  }
  if(water_timer_sw) {
    if(waterTimerEnabled) lv_obj_add_state(water_timer_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(water_timer_sw, LV_STATE_CHECKED);
  }
  update_timer_labels();

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

static void schedule_next_watering(bool resetFromNow) {
  if(!waterTimerEnabled) {
    nextWateringEpoch = 0;
    nextWateringMillis = 0;
    return;
  }

  const uint64_t intervalMs = water_interval_ms();
  time_t now = time(nullptr);
  if(now > 100000) {
    if(resetFromNow || nextWateringEpoch <= now) nextWateringEpoch = now + (time_t)(intervalMs / 1000UL);
  } else {
    uint64_t monotonicMs = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if(resetFromNow || nextWateringMillis <= monotonicMs) nextWateringMillis = monotonicMs + intervalMs;
  }
}

static void send_watering_command() {
  Serial2.println("CMD:WATER_PUMP");
  sysLog("Arduino control -> CMD:WATER_PUMP");
  schedule_next_watering(true);
  uiNeedsUpdate = true;
}

void evaluateControlLogic() {
  if(millis() - lastControlCheck < 5000UL) return;

  lastControlCheck = millis();

  if(waterTimerEnabled) {
    time_t now = time(nullptr);
    if(now > 100000) {
      if(nextWateringEpoch <= now) send_watering_command();
    } else if(nextWateringMillis > 0 && (uint64_t)(esp_timer_get_time() / 1000ULL) >= nextWateringMillis) {
      send_watering_command();
    }
  }

  if(lastSerialRecv == 0 || millis() - lastSerialRecv > 15000UL) return;

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
    if(parseTaggedFloat(data, "PH:", value) || parseTaggedFloat(data, "pH:", value) || parseTaggedFloat(data, "P:", value)) {
      livePh = fminf(fmaxf(value, 0.0f), 14.0f);
      Serial.print("  -> livePh = "); Serial.println(livePh);
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
  create_network_settings_page();
  configure_dashboard_ph_widgets();

  if(tabview) {
    lv_obj_add_event_cb(tabview, tabview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t * tab_btns = lv_tabview_get_tab_btns(tabview);
    if(tab_btns != NULL) lv_obj_add_event_cb(tab_btns, tabview_tab_buttons_cb, LV_EVENT_CLICKED, NULL);
  }

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
  if(temp_slider) lv_obj_add_flag(temp_slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(hum_slider) lv_obj_add_flag(hum_slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(soil_slider) lv_obj_add_flag(soil_slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(temp_slider) lv_obj_add_flag(temp_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(hum_slider) lv_obj_add_flag(hum_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(soil_slider) lv_obj_add_flag(soil_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(temp_slider_label) lv_obj_add_flag(temp_slider_label, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(hum_slider_label) lv_obj_add_flag(hum_slider_label, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(soil_slider_label) lv_obj_add_flag(soil_slider_label, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  if(temp_slider_label) lv_obj_add_flag(temp_slider_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(hum_slider_label) lv_obj_add_flag(hum_slider_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(soil_slider_label) lv_obj_add_flag(soil_slider_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if(lux_slider) {
    lv_slider_set_range(lux_slider, 0, 40000);
    lv_obj_add_flag(lux_slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(lux_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(lux_slider, lux_slider_cb, LV_EVENT_ALL, NULL);
  }

  uiNeedsUpdate = true;
  update_ui_from_data();

  build_time_picker_modal();
  build_water_timer_modal();
  schedule_next_watering(true);

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