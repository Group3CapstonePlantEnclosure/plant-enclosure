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

// ==========================================
// UI & LOGIC
// ==========================================

void build_lvgl_ui() {
  // Simple modern dashboard label
  lv_obj_t * label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "S3 Dashboard\nWaiting for Data...");
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
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