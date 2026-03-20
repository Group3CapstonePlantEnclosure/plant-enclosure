#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
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
static lv_color_t buf[320 * 10]; 

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

// --- PIN DEFINITIONS ---
#define TOUCH_IRQ  1  
#define SD_CS      5
#define TFT_BL    38  

#ifndef RXD2
#define RXD2 16
#endif
#ifndef TXD2
#define TXD2 17
#endif

// Backlight PWM Settings
const int pwmFreq = 5000;
const int pwmResolution = 8;
const int pwmChannel = 0;
volatile bool isAsleep = false;

TFT_eSPI tft = TFT_eSPI();

static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

// --- SYSTEM GLOBALS ---
float liveTemp = 0.0, liveHum = 0.0, liveLux = 0.0;
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
unsigned long lastMinuteTick = 0, lastSerialRecv = 0;

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

// ==========================================
// CALIBRATION ROUTINE
// ==========================================
void touch_calibrate() {
  uint16_t calibrationData[5];
  uint8_t calDataOK = 0;

  if (!SPIFFS.begin()) {
    Serial.println("Formatting SPIFFS...");
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
    tft.setTouch(calibrationData);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(20, 0);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("Touch corners as indicated");
    tft.calibrateTouch(calibrationData, TFT_MAGENTA, TFT_BLACK, 15);
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
  ledcWrite(pwmChannel, brightness); 
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  uint16_t touchX, touchY;
  bool touched = tft.getTouch(&touchX, &touchY);

  if (touched) {
    data->point.x = touchX;
    data->point.y = touchY;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ==========================================
// SQUARELINE C++ BRIDGES
// ==========================================

// 1. THE GLOBAL BACK BUTTON LOGIC
void global_back_cb(lv_event_t * e) {
    // Hide all possible sub-pages
    if(ui_env_page != NULL) lv_obj_add_flag(ui_env_page, LV_OBJ_FLAG_HIDDEN);
    if(ui_lighting_page != NULL) lv_obj_add_flag(ui_lighting_page, LV_OBJ_FLAG_HIDDEN);
    if(ui_display_page != NULL) lv_obj_add_flag(ui_display_page, LV_OBJ_FLAG_HIDDEN);
    
    // Hide the back button itself
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

    // Show the main menu list
    if(ui_menu_list != NULL) lv_obj_clear_flag(ui_menu_list, LV_OBJ_FLAG_HIDDEN);
}

// 2. OPEN WIFI SCANNER
void open_wifi_scanner_cb(lv_event_t * e) {
  sysLog("Opening C++ WiFi Scanner...");
  build_wifi_scanner_ui(); 
}

// ==========================================
// CONTROL CALLBACKS (Assigned in SquareLine Events)
// ==========================================

// NOTE: You must assign these function names to your UI widgets in SquareLine!
void temp_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    tempLow = lv_slider_get_left_value(slider);
    tempHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
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

void dark_mode_switch_cb(lv_event_t * e) {
  lv_obj_t * sw = lv_event_get_target(e);
  isDarkMode = lv_obj_has_state(sw, LV_STATE_CHECKED);
  // (In SquareLine, it is better to handle theme changes via the built-in Theme manager, but this keeps your variable synced)
  triggerCloudPush = true;
}

// ==========================================
// WIFI & TIME PICKER UI (Custom C++ Overlays)
// ==========================================

void build_wifi_scanner_ui() {
  if(wifi_list != NULL) { lv_obj_del(wifi_list); wifi_list = NULL; }
  
  // Hide SquareLine UI while scanning
  if(tabview != NULL) lv_obj_add_flag(tabview, LV_OBJ_FLAG_HIDDEN);

  if(CloudTaskHandle != NULL) vTaskSuspend(CloudTaskHandle); 

  Serial.println("Scanning...");
  int n = WiFi.scanNetworks(false, true); 

  if(CloudTaskHandle != NULL) vTaskResume(CloudTaskHandle);

  wifi_list = lv_list_create(lv_scr_act());
  lv_obj_set_size(wifi_list, 320, 210);
  lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, 0);
  
  lv_obj_t * back_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_LEFT, "Back to Settings");
  lv_obj_add_event_cb(back_btn, [](lv_event_t * e){ 
    lv_obj_del(wifi_list); wifi_list = NULL; 
    if(tabview != NULL) lv_obj_clear_flag(tabview, LV_OBJ_FLAG_HIDDEN); 
  }, LV_EVENT_CLICKED, NULL);

  lv_list_add_text(wifi_list, "Available Networks");
  if (n == 0) {
    lv_list_add_btn(wifi_list, LV_SYMBOL_REFRESH, "No Networks Found.");
  } else {
    for (int i = 0; i < n; ++i) {
      // Basic implementation for scanning list
      lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, WiFi.SSID(i).c_str());
    }
  }
}

void build_time_picker_modal() {
  // Keeping your original time picker logic intact.
  time_picker_bg = lv_obj_create(lv_scr_act());
  lv_obj_set_size(time_picker_bg, 320, 240);
  lv_obj_center(time_picker_bg);
  lv_obj_set_style_bg_color(time_picker_bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(time_picker_bg, LV_OPA_80, 0);
  lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN); 

  lv_obj_t * modal = lv_obj_create(time_picker_bg);
  lv_obj_set_size(modal, 280, 200);
  lv_obj_center(modal);
  
  lv_obj_t * title = lv_label_create(modal);
  lv_obj_set_user_data(time_picker_bg, title); 
  lv_label_set_text(title, "SELECT TIME");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);
  
  lv_obj_t * cancel_btn = lv_btn_create(modal);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -60, 10);
  lv_obj_t * cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, "CANCEL");
  lv_obj_add_event_cb(cancel_btn, [](lv_event_t * e){ lv_obj_add_flag(time_picker_bg, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// BACKGROUND ARDUINO & CLOUD TASKS
// ==========================================

void update_ui_from_data() {
  if(!uiNeedsUpdate) return;
  uiNeedsUpdate = false;
  char buf[64];

  // 1. Update Dashboard Labels (Ensure these are named ui_temp_label etc. in SquareLine)
  if(ui_temp_label) {
    snprintf(buf, sizeof(buf), "Temp: %.1f \xb0%s", liveTemp, useFahrenheit ? "F" : "C");
    lv_label_set_text(ui_temp_label, buf);
  }
  if(ui_hum_label) {
    snprintf(buf, sizeof(buf), "Humidity: %.0f %%", liveHum);
    lv_label_set_text(ui_hum_label, buf);
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

  // 2. Update Environment Sliders
  if(temp_slider) {
    lv_slider_set_left_value(temp_slider, useFahrenheit ? tempLow : (tempLow - 32.0) * 5.0 / 9.0, LV_ANIM_OFF);
    lv_slider_set_value(temp_slider, useFahrenheit ? tempHigh : (tempHigh - 32.0) * 5.0 / 9.0, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
    if(temp_slider_label) lv_label_set_text(temp_slider_label, buf);
  }
  if(hum_slider) {
    lv_slider_set_left_value(hum_slider, humLow, LV_ANIM_OFF);
    lv_slider_set_value(hum_slider, humHigh, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Humidity: %.0f - %.0f %%", humLow, humHigh);
    if(hum_slider_label) lv_label_set_text(hum_slider_label, buf);
  }

  // 3. Update Switches
  if(timer_sw) {
    if(timerEnabled) lv_obj_add_state(timer_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(timer_sw, LV_STATE_CHECKED);
  } 
  if(dm_sw) {
    if(isDarkMode) lv_obj_add_state(dm_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(dm_sw, LV_STATE_CHECKED);
  }
}

// ... (Keep your syncWithCloudSilent, checkSerialSensors, and evaluateControlLogic here EXACTLY as they were) ...

void uiLoopTask(void * parameter) {
  for(;;) {
    lv_timer_handler();
    if(uiNeedsUpdate) update_ui_from_data();
    
    // Periodically flag the UI to update the live dashboard data
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

  ledcSetup(pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(TFT_BL, pwmChannel);
  setBacklight(255); 

  tft.begin();
  tft.setRotation(1);
  tft.setSwapBytes(true); 
  tft.fillScreen(TFT_BLACK);

  touch_calibrate();

  // --- Initialize LVGL v8 ---
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

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

  // --- MAP SQUARELINE OBJECTS TO ARDUINO VARIABLES ---
  tabview = ui_TabView1; 
  temp_slider = ui_temp_slider;
  hum_slider = ui_hum_slider;
  // NOTE: If you haven't added these to SquareLine yet, leave them commented!
  // soil_slider = ui_soil_slider;
  // lux_slider = ui_lux_slider;
  // temp_slider_label = ui_temp_slider_label;
  // hum_slider_label = ui_hum_slider_label;
  // timer_sw = ui_timer_sw;
  // dm_sw = ui_dm_sw;

  build_time_picker_modal();

  xTaskCreatePinnedToCore(uiLoopTask, "UITask", 16384, NULL, 2, &UILoopTaskHandle, 1);

  // --- WIFI BOOT ROUTINE ---
  isConnecting = true; 
  WiFi.mode(WIFI_STA);
  // (Keep your existing fast-boot connection logic here)
  isConnecting = false; 

  // xTaskCreatePinnedToCore(cloudTask, "CloudTask", 8192, NULL, 1, &CloudTaskHandle, 0);
}

void loop() {
  // checkSerialSensors();
  // evaluateControlLogic();
  delay(10); 
}