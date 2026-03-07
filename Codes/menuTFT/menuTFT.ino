#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <time.h>

// --- DISPLAY & TOUCH LIBRARIES ---
#include <lvgl.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

// --- DASHBOARD UI VARIABLES ---
lv_obj_t * temp_label;
lv_obj_t * hum_label;
lv_obj_t * lux_label;

// --- WIFI SETUP UI VARIABLES ---
lv_obj_t * wifi_list;
lv_obj_t * pwd_ta;
lv_obj_t * kb;
char selected_ssid[64];

// --- PIN DEFINITIONS (From your working sketch) ---
#define TFT_DC    9
#define TFT_CS    10
#define TFT_RST   8
#define TFT_MOSI  11
#define TFT_CLK   12
#define TFT_MISO  13
#define TOUCH_CS  14

// --- SERIAL CONFIG ---
#define RXD2 16
#define TXD2 17

// --- DISPLAY OBJECTS ---
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS);

static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
static uint8_t draw_buf[screenWidth * 10 * 2]; 

// --- SYSTEM GLOBALS ---
float liveTemp = 0.0, liveHum = 0.0, liveLux = 0.0;
float tempLow = 68.0, tempHigh = 77.0, luxThreshold = 30000;
bool timerEnabled = false;
int timeOnHour = 8, timeOnMinute = 0, timeOffHour = 20, timeOffMinute = 0;
int currentHour = 12, currentMinute = 0, globalBrightness = 10;
unsigned long lastMinuteTick = 0, lastSerialRecv = 0;
volatile bool triggerCloudPush = false;
TaskHandle_t CloudTaskHandle;
const char* firebaseURL = "https://plant-enclosure-default-rtdb.firebaseio.com/settings.json";

// --- WIFI CREDENTIALS ---
struct WiFiCreds { const char* ssid; const char* pass; };
WiFiCreds myNetworks[] = {
  { "MASCU-Fi", "milliondollarbackyard@1192" }, // Home WiFi
  { "shanel", "jamescharles" }                  // Phone Hotspot
};

// ==========================================
// LVGL v9 CALLBACKS (Using your proven drivers)
// ==========================================

void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  
  // Use Adafruit's RGB bitmap draw function to push the LVGL buffer
  tft.drawRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  
  lv_display_flush_ready(display);
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    
    // Using your specific calibration mapping logic!
    data->point.x = map(p.x, 3750, 350, 0, 320);
    data->point.y = map(p.y, 3600, 280, 0, 240);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Timer callback to update the dashboard labels every second
void update_dashboard_timer_cb(lv_timer_t * timer) {
  // Only update if the labels exist
  if (temp_label && hum_label && lux_label) {
    char buf[32];
    
    snprintf(buf, sizeof(buf), "Temp: %.1f °F", liveTemp);
    lv_label_set_text(temp_label, buf);
    
    snprintf(buf, sizeof(buf), "Humidity: %.0f %%", liveHum);
    lv_label_set_text(hum_label, buf);
    
    snprintf(buf, sizeof(buf), "Light: %.0f lux", liveLux);
    lv_label_set_text(lux_label, buf);
  }
}

// Callback for the WiFi scan button
static void scan_wifi_btn_cb(lv_event_t * e) {
  build_wifi_scanner_ui();
}

// ==========================================
// UI & LOGIC
// ==========================================

void build_lvgl_ui() {
  // 1. Create a Tab View
  lv_obj_t * tabview = lv_tabview_create(lv_scr_act());
  
  // 2. Add 2 Tabs
  lv_obj_t * tab1 = lv_tabview_add_tab(tabview, "Dashboard");
  lv_obj_t * tab2 = lv_tabview_add_tab(tabview, "Settings");

  // --- TAB 1: DASHBOARD ---
  // Temperature
  temp_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_24, 0); // Make it big
  lv_label_set_text(temp_label, "Temp: --.- °F");
  lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 20);

  // Humidity
  hum_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(hum_label, &lv_font_montserrat_24, 0);
  lv_label_set_text(hum_label, "Humidity: -- %");
  lv_obj_align(hum_label, LV_ALIGN_TOP_MID, 0, 60);

  // Lux
  lux_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(lux_label, &lv_font_montserrat_24, 0);
  lv_label_set_text(lux_label, "Light: -- lux");
  lv_obj_align(lux_label, LV_ALIGN_TOP_MID, 0, 100);

  // Create an LVGL timer to update these labels every 1000ms (1 second)
  lv_timer_create(update_dashboard_timer_cb, 1000, NULL);


  // --- TAB 2: SETTINGS ---
  // Create a button to launch the WiFi Scanner
  lv_obj_t * wifi_btn = lv_button_create(tab2);
  lv_obj_set_size(wifi_btn, 200, 50);
  lv_obj_align(wifi_btn, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_add_event_cb(wifi_btn, scan_wifi_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t * wifi_btn_label = lv_label_create(wifi_btn);
  lv_label_set_text(wifi_btn_label, "Scan WiFi Networks");
  lv_obj_center(wifi_btn_label);
}

void checkSerialSensors() {
  while (Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if (data.indexOf("T:") != -1 && data.indexOf("H:") != -1) {
      int tIdx = data.indexOf("T:"), hIdx = data.indexOf("H:"), lIdx = data.indexOf("L:");
      liveTemp = data.substring(tIdx + 2, data.indexOf(',', tIdx)).toFloat();
      liveHum = data.substring(hIdx + 2, data.indexOf(',', hIdx)).toFloat();
      liveLux = data.substring(lIdx + 2).toFloat();
      lastSerialRecv = millis();
    }
  }
}

void evaluateControlLogic() {
  if (liveTemp > tempHigh) Serial2.println("CMD:COOL");
  else if (liveTemp < tempLow) Serial2.println("CMD:HEAT");
  else Serial2.println("CMD:PELTIER_OFF");

  bool turnLightOn = (liveLux < luxThreshold);
  if (timerEnabled) {
    int now = (currentHour * 60) + currentMinute;
    int on = (timeOnHour * 60) + timeOnMinute, off = (timeOffHour * 60) + timeOffMinute;
    bool inWin = (on < off) ? (now >= on && now < off) : (now >= on || now < off);
    if (!inWin) turnLightOn = false;
  }
  Serial2.printf("CMD:LIGHT,%d\n", turnLightOn ? map(globalBrightness, 1, 10, 25, 255) : 0);
}

// ==========================================
// CLOUD TASK (CORE 0)
// ==========================================

void cloudTask(void * parameter) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (triggerCloudPush) {
        HTTPClient http; http.begin(firebaseURL); http.addHeader("Content-Type", "application/json");
        StaticJsonDocument<512> doc;
        doc["tempLow"] = tempLow; doc["tempHigh"] = tempHigh; doc["luxThreshold"] = luxThreshold;
        String output; serializeJson(doc, output);
        http.sendRequest("PATCH", output); http.end();
        triggerCloudPush = false;
      }
      // Periodic live data push
      static unsigned long lastPush = 0;
      if (millis() - lastPush > 10000) {
        lastPush = millis();
        HTTPClient http; http.begin(firebaseURL); http.addHeader("Content-Type", "application/json");
        StaticJsonDocument<200> doc; doc["liveTemp"] = liveTemp; doc["liveHum"] = liveHum; doc["liveLux"] = liveLux;
        String output; serializeJson(doc, output);
        http.sendRequest("PATCH", output); http.end();
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// 3. Callback for when the user hits "Enter" on the keyboard
static void kb_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_READY) { // User pressed the Checkmark/Enter button
    const char * password = lv_textarea_get_text(pwd_ta);
    
    Serial.print("Attempting to connect to: ");
    Serial.println(selected_ssid);
    Serial.print("With Password: ");
    Serial.println(password);

    // Tell the ESP32 to connect
    WiFi.disconnect();
    WiFi.begin(selected_ssid, password);
    
    // Clean up the UI
    lv_obj_del(pwd_ta);
    lv_obj_del(kb);
    
    // You could pop up a "Connecting..." label here!
  }
}

// 2. Callback for when a network is tapped in the list
static void list_btn_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_CLICKED) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    const char * ssid = lv_list_get_btn_text(wifi_list, btn);
    strcpy(selected_ssid, ssid); // Save the clicked network name
    
    // Hide the list
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);

    // Create a text area for the password
    pwd_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_password_mode(pwd_ta, true); // Hides characters like ***
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_width(pwd_ta, lv_pct(90));
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_MID, 0, 10);
    lv_textarea_set_placeholder_text(pwd_ta, "Enter Password");

    // Create the keyboard
    kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, pwd_ta); // Link keyboard to text area
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, NULL);
  }
}

// 1. Main function to scan and show the UI
void build_wifi_scanner_ui() {
  Serial.println("Scanning for networks...");
  int n = WiFi.scanNetworks();
  
  // Create the list widget
  wifi_list = lv_list_create(lv_scr_act());
  lv_obj_set_size(wifi_list, lv_pct(100), lv_pct(100));
  lv_obj_center(wifi_list);
  
  lv_list_add_text(wifi_list, "Available Networks");

  if (n == 0) {
    lv_list_add_text(wifi_list, "No networks found.");
  } else {
    for (int i = 0; i < n; ++i) {
      // Add a button for each network found
      lv_obj_t * btn = lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, WiFi.SSID(i).c_str());
      lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }
  }
}

// ==========================================
// SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);

  // Initialize custom SPI for ESP32-S3
  SPI.begin(TFT_CLK, TFT_MISO, TFT_MOSI);

  // Start Hardware
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  ts.begin(SPI);
  ts.setRotation(1);

  // LVGL Init
  lv_init();
  lv_tick_set_cb(millis);

  lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  build_lvgl_ui();

// --- 4. CONNECT TO WIFI ---
  WiFi.mode(WIFI_STA);
  Serial.println("\nConnecting to WiFi...");
  
  bool connected = false;
  int numNetworks = sizeof(myNetworks) / sizeof(myNetworks[0]);
  
  for (int i = 0; i < numNetworks; i++) {
    Serial.print("Trying Network: ");
    Serial.println(myNetworks[i].ssid);
    
    WiFi.disconnect(); 
    WiFi.begin(myNetworks[i].ssid, myNetworks[i].pass);
    
    // Wait up to 5 seconds per network
    for (int k = 0; k < 100; k++) { 
      if (WiFi.status() == WL_CONNECTED) { 
        connected = true; 
        break; 
      } 
      delay(50); 
    }
    
    if (connected) {
      Serial.print("Connected successfully! IP: ");
      Serial.println(WiFi.localIP());
      break; 
    }
  }

  if (!connected) {
    Serial.println("Failed to connect to any WiFi. Running in offline mode.");
  }

  // Background Task
  xTaskCreatePinnedToCore(cloudTask, "CloudTask", 8192, NULL, 1, &CloudTaskHandle, 0);
}

void loop() {
  lv_timer_handler();
  checkSerialSensors();
  
  static unsigned long lastLogic = 0;
  if (millis() - lastLogic > 5000) {
    lastLogic = millis();
    evaluateControlLogic();
  }
  delay(5);
}