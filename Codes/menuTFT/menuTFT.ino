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

// --- GLOBALS FOR DISPLAY ---
lv_display_t * disp; 

// --- DASHBOARD UI VARIABLES ---
lv_obj_t * status_bar;
lv_obj_t * wifi_status_label;
lv_obj_t * temp_label;
lv_obj_t * hum_label;
lv_obj_t * lux_label;
lv_obj_t * tabview;
lv_obj_t * menu; 

// --- MENU UI VARIABLES ---
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

// --- WIFI SETUP UI VARIABLES ---
lv_obj_t * wifi_list = NULL;
lv_obj_t * pwd_screen = NULL;
lv_obj_t * pwd_ta;
lv_obj_t * kb;
lv_obj_t * conn_status_label;
char selected_ssid[64];

void build_wifi_scanner_ui(); 

// --- PIN DEFINITIONS ---
#define TFT_DC    9
#define TFT_CS    10
#define TFT_RST   8
#define TFT_MOSI  11
#define TFT_CLK   12
#define TFT_MISO  13
#define TOUCH_CS  14

#define RXD2 16
#define TXD2 17

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS);

static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
static uint8_t draw_buf[screenWidth * 10 * 2]; 

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
int timeZoneOffset = -5; // Default EST

String webLogBuffer = "Device Booted. System initialized.\n";
unsigned long lastMinuteTick = 0, lastSerialRecv = 0;

volatile bool triggerCloudPush = false;
volatile bool uiNeedsUpdate = false; // Tells Loop() that Web made a change
TaskHandle_t CloudTaskHandle;
const char* firebaseURL = "https://plant-enclosure-default-rtdb.firebaseio.com/settings.json";

struct WiFiCreds { const char* ssid; const char* pass; };
WiFiCreds myNetworks[] = {
  { "MASCU-Fi", "milliondollarbackyard@1192" },
  { "shanel", "jamescharles" }                  
};

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
// LVGL v9 CALLBACKS 
// ==========================================

void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.drawRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(display);
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    data->point.x = map(p.x, 3750, 350, 0, 320);
    data->point.y = map(p.y, 3600, 280, 0, 240);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ==========================================
// TIMER & UI CALLBACKS
// ==========================================

void update_dashboard_timer_cb(lv_timer_t * timer) {
  if (temp_label && hum_label && lux_label) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Temp: %.1f \xb0%s", liveTemp, useFahrenheit ? "F" : "C");
    lv_label_set_text(temp_label, buf);
    
    snprintf(buf, sizeof(buf), "Humidity: %.0f %%", liveHum);
    lv_label_set_text(hum_label, buf);
    
    snprintf(buf, sizeof(buf), "Light: %.0f lux", liveLux);
    lv_label_set_text(lux_label, buf);
  }

  if (wifi_status_label != NULL) {
    if(WiFi.status() == WL_CONNECTED) {
      long rssi = WiFi.RSSI();
      lv_color_t color;
      
      if(rssi > -65) { color = lv_color_hex(0x00FF00); }      
      else if(rssi > -75){ color = lv_color_hex(0xFFFF00); } 
      else { color = lv_color_hex(0xFFA500); }               

      lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI);
      lv_obj_set_style_text_color(wifi_status_label, color, 0);
    } else {
      lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFF0000), 0);
    }
  }
}

static void scan_wifi_btn_cb(lv_event_t * e) { build_wifi_scanner_ui(); }

static void dark_mode_switch_cb(lv_event_t * e) {
  lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
  isDarkMode = lv_obj_has_state(sw, LV_STATE_CHECKED);
  
  lv_theme_t * theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_CYAN), isDarkMode, LV_FONT_DEFAULT);
  lv_display_set_theme(disp, theme);
  triggerCloudPush = true; // Sync to DB!
}

static void unit_switch_cb(lv_event_t * e) {
  lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
  bool f_selected = lv_obj_has_state(sw, LV_STATE_CHECKED);

  if(f_selected != useFahrenheit) {
    useFahrenheit = f_selected;
    if(useFahrenheit) {
      tempLow = (tempLow * 9.0 / 5.0) + 32.0;
      tempHigh = (tempHigh * 9.0 / 5.0) + 32.0;
      liveTemp = (liveTemp * 9.0 / 5.0) + 32.0;
      lv_slider_set_range(temp_slider, 40, 100);
    } else {
      tempLow = (tempLow - 32.0) * 5.0 / 9.0;
      tempHigh = (tempHigh - 32.0) * 5.0 / 9.0;
      liveTemp = (liveTemp - 32.0) * 5.0 / 9.0;
      lv_slider_set_range(temp_slider, 4, 38);
    }
    
    lv_slider_set_left_value(temp_slider, tempLow, LV_ANIM_OFF);
    lv_slider_set_value(temp_slider, tempHigh, LV_ANIM_OFF);

    char buf[64];
    snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
    lv_label_set_text(temp_slider_label, buf);
    triggerCloudPush = true;
  }
}

// --- RANGE SLIDER CALLBACKS ---

static void temp_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    tempLow = lv_slider_get_left_value(slider);
    tempHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
    lv_label_set_text(temp_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

static void hum_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    humLow = lv_slider_get_left_value(slider);
    humHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Humidity: %.0f - %.0f %%", humLow, humHigh);
    lv_label_set_text(hum_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

static void soil_range_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    soilLow = lv_slider_get_left_value(slider);
    soilHigh = lv_slider_get_value(slider);
    char buf[64]; snprintf(buf, sizeof(buf), "Moisture: %.0f - %.0f %%", soilLow, soilHigh);
    lv_label_set_text(soil_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

static void lux_slider_cb(lv_event_t * e) {
  lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
    luxThreshold = lv_slider_get_value(slider) * 1000; 
    char buf[64]; snprintf(buf, sizeof(buf), "Lux Limit: %.0f", luxThreshold);
    lv_label_set_text(lux_slider_label, buf);
  }
  if(code == LV_EVENT_RELEASED) triggerCloudPush = true; 
}

// ==========================================
// WIFI CONNECTION UI 
// ==========================================
static void pwd_eye_click_cb(lv_event_t * e) {
  bool is_pwd = lv_textarea_get_password_mode(pwd_ta);
  lv_textarea_set_password_mode(pwd_ta, !is_pwd); 
  lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t * label = lv_obj_get_child(btn, 0);
  if(is_pwd) lv_label_set_text(label, LV_SYMBOL_EYE_CLOSE);
  else lv_label_set_text(label, LV_SYMBOL_EYE_OPEN);
}

static void kb_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_CANCEL) {
    lv_obj_del(pwd_screen); pwd_screen = NULL; build_wifi_scanner_ui(); return;
  }
  if(code == LV_EVENT_READY) { 
    const char * password = lv_textarea_get_text(pwd_ta);
    if(strlen(password) == 0) { lv_label_set_text(conn_status_label, "Error: Password cannot be empty!"); return; }
    lv_label_set_text(conn_status_label, "Connecting... Please wait.");
    WiFi.disconnect(); WiFi.begin(selected_ssid, password);
    int retry = 0; while (WiFi.status() != WL_CONNECTED && retry < 10) { delay(500); retry++; }
    if(WiFi.status() == WL_CONNECTED) {
      lv_label_set_text(conn_status_label, "Connected Successfully!");
      sysLog("WiFi Connected to " + String(selected_ssid));
    } else {
      lv_label_set_text(conn_status_label, "Failed to connect.");
    }

    lv_timer_t * timer = lv_timer_create([](lv_timer_t * t){
      if(pwd_screen != NULL) { lv_obj_del(pwd_screen); pwd_screen = NULL; }
      if(tabview != NULL) { lv_obj_remove_flag(tabview, LV_OBJ_FLAG_HIDDEN); }
      lv_timer_del(t); 
    }, 2000, NULL);
  }
}

static void list_btn_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_CLICKED) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    const char * ssid = lv_list_get_btn_text(wifi_list, btn);
    strcpy(selected_ssid, ssid); 
    lv_obj_del(wifi_list); wifi_list = NULL;

    pwd_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pwd_screen, 320, 210);
    lv_obj_align(pwd_screen, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t * back_btn = lv_btn_create(pwd_screen);
    lv_obj_set_size(back_btn, 40, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT); lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, [](lv_event_t * e){
      lv_obj_del(pwd_screen); pwd_screen = NULL; build_wifi_scanner_ui(); 
    }, LV_EVENT_CLICKED, NULL);

    conn_status_label = lv_label_create(pwd_screen);
    lv_label_set_text_fmt(conn_status_label, "Network: %s", selected_ssid);
    lv_obj_align(conn_status_label, LV_ALIGN_TOP_MID, 0, 5);

    pwd_ta = lv_textarea_create(pwd_screen);
    lv_textarea_set_password_mode(pwd_ta, true); 
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_size(pwd_ta, 220, 40); 
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_textarea_set_placeholder_text(pwd_ta, "Enter Password");

    lv_obj_t * eye_btn = lv_btn_create(pwd_screen);
    lv_obj_set_size(eye_btn, 45, 40);
    lv_obj_align_to(eye_btn, pwd_ta, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_t * eye_label = lv_label_create(eye_btn);
    lv_label_set_text(eye_label, LV_SYMBOL_EYE_OPEN); lv_obj_center(eye_label);
    lv_obj_add_event_cb(eye_btn, pwd_eye_click_cb, LV_EVENT_CLICKED, NULL);

    kb = lv_keyboard_create(pwd_screen);
    lv_keyboard_set_textarea(kb, pwd_ta);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, NULL); 
  }
}

void build_wifi_scanner_ui() {
  if(wifi_list != NULL) lv_obj_del(wifi_list);
  lv_obj_add_flag(tabview, LV_OBJ_FLAG_HIDDEN);

  int n = WiFi.scanNetworks();
  wifi_list = lv_list_create(lv_scr_act());
  lv_obj_set_size(wifi_list, 320, 210);
  lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, 0);
  
  lv_obj_t * back_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_LEFT, "Back to Settings");
  lv_obj_add_event_cb(back_btn, [](lv_event_t * e){ 
    lv_obj_del(wifi_list); wifi_list = NULL; lv_obj_remove_flag(tabview, LV_OBJ_FLAG_HIDDEN); 
  }, LV_EVENT_CLICKED, NULL);

  lv_list_add_text(wifi_list, "Available Networks");
  if (n == 0) {
    lv_list_add_btn(wifi_list, LV_SYMBOL_REFRESH, "No Networks Found.");
  } else {
    for (int i = 0; i < n; ++i) {
      lv_obj_t * btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, WiFi.SSID(i).c_str());
      lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }
  }
}

// Helper to draw nice borders around menu items
void apply_menu_border(lv_obj_t * cont) {
  lv_obj_set_style_border_width(cont, 2, 0);
  lv_obj_set_style_border_color(cont, lv_color_hex(0x888888), 0);
  lv_obj_set_style_radius(cont, 8, 0);
  lv_obj_set_style_margin_all(cont, 4, 0);
}

// ==========================================
// MAIN UI BUILDER
// ==========================================

void build_lvgl_ui() {
  status_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(status_bar, 320, 30);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(status_bar, 5, 0);        
  lv_obj_set_style_radius(status_bar, 0, 0);         
  lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

  wifi_status_label = lv_label_create(status_bar);
  lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI " " LV_SYMBOL_CLOSE); 
  lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFF0000), 0);
  lv_obj_align(wifi_status_label, LV_ALIGN_RIGHT_MID, -5, 0);

  tabview = lv_tabview_create(lv_scr_act());
  lv_obj_set_size(tabview, 320, 210); 
  lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
  
  lv_obj_t * tv_cont = lv_tabview_get_content(tabview);
  lv_obj_clear_flag(tv_cont, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * tab_btns = lv_tabview_get_tab_bar(tabview);
  lv_obj_set_height(tab_btns, 35); 
  lv_obj_set_style_pad_all(tab_btns, 0, 0); 
  lv_obj_set_style_pad_top(tab_btns, 5, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(tab_btns, 5, LV_PART_ITEMS);
  lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_14, 0); 
  
  lv_obj_t * tab1 = lv_tabview_add_tab(tabview, "Dashboard");
  lv_obj_t * tab2 = lv_tabview_add_tab(tabview, "Menu");

  // --- TAB 1: DASHBOARD ---
  temp_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_18, 0); 
  lv_label_set_text(temp_label, "Temp: --.- °F");
  lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 20);

  hum_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(hum_label, &lv_font_montserrat_18, 0);
  lv_label_set_text(hum_label, "Humidity: -- %");
  lv_obj_align(hum_label, LV_ALIGN_TOP_MID, 0, 60);

  lux_label = lv_label_create(tab1);
  lv_obj_set_style_text_font(lux_label, &lv_font_montserrat_18, 0);
  lv_label_set_text(lux_label, "Light: -- lux");
  lv_obj_align(lux_label, LV_ALIGN_TOP_MID, 0, 100);

  lv_timer_create(update_dashboard_timer_cb, 1000, NULL);


  // --- TAB 2: MENU SYSTEM ---
  lv_obj_set_style_pad_all(tab2, 0, 0); 
  menu = lv_menu_create(tab2);
  lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
  lv_menu_set_mode_header(menu, LV_MENU_HEADER_TOP_FIXED); 

  lv_obj_t * sub_network = lv_menu_page_create(menu, "Network Settings");
  lv_obj_t * sub_display = lv_menu_page_create(menu, "Display Settings");
  lv_obj_t * sub_env = lv_menu_page_create(menu, "Environment Config");

  // 1. Build Network Sub-Page
  lv_obj_t * cont = lv_menu_cont_create(sub_network);
  apply_menu_border(cont); // Added border
  lv_obj_t * wifi_btn = lv_button_create(cont);
  lv_obj_add_event_cb(wifi_btn, scan_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(wifi_btn), "Scan WiFi Networks");

  // 2. Build Display Sub-Page
  cont = lv_menu_cont_create(sub_display);
  apply_menu_border(cont); // Added border
  lv_label_set_text(lv_label_create(cont), "Dark Mode");
  dm_sw = lv_switch_create(cont);
  if(isDarkMode) lv_obj_add_state(dm_sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(dm_sw, dark_mode_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t * cont2 = lv_menu_cont_create(sub_display);
  apply_menu_border(cont2); // Added border
  lv_label_set_text(lv_label_create(cont2), "Use Fahrenheit (\xb0""F)");
  unit_sw = lv_switch_create(cont2);
  if(useFahrenheit) lv_obj_add_state(unit_sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(unit_sw, unit_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // 3. Build Environment Sub-Page
  char buf[64];

  lv_obj_t * cont_t = lv_menu_cont_create(sub_env);
  apply_menu_border(cont_t); // Added border to group
  lv_obj_set_flex_flow(cont_t, LV_FLEX_FLOW_COLUMN); 
  temp_slider_label = lv_label_create(cont_t);
  snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
  lv_label_set_text(temp_slider_label, buf);
  temp_slider = lv_slider_create(cont_t);
  lv_slider_set_mode(temp_slider, LV_SLIDER_MODE_RANGE); 
  lv_slider_set_range(temp_slider, useFahrenheit ? 40 : 4, useFahrenheit ? 100 : 38); 
  lv_slider_set_value(temp_slider, tempHigh, LV_ANIM_OFF);      
  lv_slider_set_left_value(temp_slider, tempLow, LV_ANIM_OFF);  
  lv_obj_set_width(temp_slider, 240);
  lv_obj_add_event_cb(temp_slider, temp_range_slider_cb, LV_EVENT_ALL, NULL);

  lv_obj_t * cont_h = lv_menu_cont_create(sub_env);
  apply_menu_border(cont_h);
  lv_obj_set_flex_flow(cont_h, LV_FLEX_FLOW_COLUMN); 
  hum_slider_label = lv_label_create(cont_h);
  snprintf(buf, sizeof(buf), "Humidity: %.0f - %.0f %%", humLow, humHigh);
  lv_label_set_text(hum_slider_label, buf);
  hum_slider = lv_slider_create(cont_h);
  lv_slider_set_mode(hum_slider, LV_SLIDER_MODE_RANGE); 
  lv_slider_set_range(hum_slider, 0, 100); 
  lv_slider_set_value(hum_slider, humHigh, LV_ANIM_OFF);
  lv_slider_set_left_value(hum_slider, humLow, LV_ANIM_OFF);
  lv_obj_set_width(hum_slider, 240);
  lv_obj_add_event_cb(hum_slider, hum_range_slider_cb, LV_EVENT_ALL, NULL);

  lv_obj_t * cont_s = lv_menu_cont_create(sub_env);
  apply_menu_border(cont_s);
  lv_obj_set_flex_flow(cont_s, LV_FLEX_FLOW_COLUMN); 
  soil_slider_label = lv_label_create(cont_s);
  snprintf(buf, sizeof(buf), "Moisture: %.0f - %.0f %%", soilLow, soilHigh);
  lv_label_set_text(soil_slider_label, buf);
  soil_slider = lv_slider_create(cont_s);
  lv_slider_set_mode(soil_slider, LV_SLIDER_MODE_RANGE); 
  lv_slider_set_range(soil_slider, 0, 100); 
  lv_slider_set_value(soil_slider, soilHigh, LV_ANIM_OFF);
  lv_slider_set_left_value(soil_slider, soilLow, LV_ANIM_OFF);
  lv_obj_set_width(soil_slider, 240);
  lv_obj_add_event_cb(soil_slider, soil_range_slider_cb, LV_EVENT_ALL, NULL);

  lv_obj_t * cont_l = lv_menu_cont_create(sub_env);
  apply_menu_border(cont_l);
  lv_obj_set_flex_flow(cont_l, LV_FLEX_FLOW_COLUMN); 
  lux_slider_label = lv_label_create(cont_l);
  snprintf(buf, sizeof(buf), "Lux Limit: %.0f", luxThreshold);
  lv_label_set_text(lux_slider_label, buf);
  lux_slider = lv_slider_create(cont_l);
  lv_slider_set_range(lux_slider, 0, 100); 
  lv_slider_set_value(lux_slider, luxThreshold / 1000, LV_ANIM_OFF);
  lv_obj_set_width(lux_slider, 240);
  lv_obj_add_event_cb(lux_slider, lux_slider_cb, LV_EVENT_ALL, NULL);
  
  // Root Menu Mapping
  lv_obj_t * main_page = lv_menu_page_create(menu, NULL);
  
  cont = lv_menu_cont_create(main_page);
  apply_menu_border(cont); 
  lv_label_set_text(lv_label_create(cont), "Temperature & Environment");
  lv_menu_set_load_page_event(menu, cont, sub_env);

  cont = lv_menu_cont_create(main_page);
  apply_menu_border(cont); 
  lv_label_set_text(lv_label_create(cont), "Display Settings");
  lv_menu_set_load_page_event(menu, cont, sub_display);

  cont = lv_menu_cont_create(main_page);
  apply_menu_border(cont); 
  lv_label_set_text(lv_label_create(cont), "Network Settings");
  lv_menu_set_load_page_event(menu, cont, sub_network);

  lv_menu_set_page(menu, main_page);
}

// ==========================================
// BACKGROUND ARDUINO & CLOUD TASKS
// ==========================================

// --- UI UPDATER (Runs on Core 1 if Cloud changes a value) ---
void update_ui_from_data() {
  if(!uiNeedsUpdate) return;
  uiNeedsUpdate = false;
  char buf[64];

  if(temp_slider) {
    lv_slider_set_left_value(temp_slider, useFahrenheit ? tempLow : (tempLow - 32.0) * 5.0 / 9.0, LV_ANIM_OFF);
    lv_slider_set_value(temp_slider, useFahrenheit ? tempHigh : (tempHigh - 32.0) * 5.0 / 9.0, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Temp: %.1f - %.1f \xb0%s", tempLow, tempHigh, useFahrenheit ? "F" : "C");
    lv_label_set_text(temp_slider_label, buf);
  }
  if(hum_slider) {
    lv_slider_set_left_value(hum_slider, humLow, LV_ANIM_OFF);
    lv_slider_set_value(hum_slider, humHigh, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Humidity: %.0f - %.0f %%", humLow, humHigh);
    lv_label_set_text(hum_slider_label, buf);
  }
  if(soil_slider) {
    lv_slider_set_left_value(soil_slider, soilLow, LV_ANIM_OFF);
    lv_slider_set_value(soil_slider, soilHigh, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Moisture: %.0f - %.0f %%", soilLow, soilHigh);
    lv_label_set_text(soil_slider_label, buf);
  }
  if(lux_slider) {
    lv_slider_set_value(lux_slider, luxThreshold / 1000, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Lux Limit: %.0f", luxThreshold);
    lv_label_set_text(lux_slider_label, buf);
  }
  if(dm_sw) {
    if(isDarkMode) lv_obj_add_state(dm_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(dm_sw, LV_STATE_CHECKED);
    lv_theme_t * theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_CYAN), isDarkMode, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);
  }
}

// --- CLOUD POLLING (Web -> ESP32) ---
void syncWithCloudSilent() {
  if(WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.begin(firebaseURL);
  int httpCode = http.GET();
  if(httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    bool changed = false;

    // --- System Actions ---
    if (doc["reboot_cmd"] == true) {
      sysLog("Web Command: Remote Reboot...");
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"reboot_cmd\":false}"); patchHttp.end();
      delay(1000); ESP.restart();
    }
    if (doc["global_reset_cmd"] == true) {
      sysLog("Web Command: Factory Reset...");
      tempLow = 68.0; tempHigh = 77.0; humLow = 40.0; humHigh = 80.0; soilLow = 30.0; soilHigh = 70.0; luxThreshold = 30000;
      timeOnHour = 8; timeOffHour = 20; timerEnabled = false; globalBrightness = 10; isDarkMode = true;
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"global_reset_cmd\":false}"); patchHttp.end();
      triggerCloudPush = true; changed = true;
    }
    if (doc["sensor_test_cmd"] == true) {
      sysLog("Web Command: Sensor test active.");
      if (tabview) lv_tabview_set_active(tabview, 0, LV_ANIM_ON); // Auto-swap to dashboard
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", "{\"sensor_test_cmd\":false}"); patchHttp.end();
    }
    if (doc["fetch_logs_cmd"] == true) {
      DynamicJsonDocument logDoc(2048); logDoc["fetch_logs_cmd"] = false; logDoc["system_logs"] = webLogBuffer;
      String logStr; serializeJson(logDoc, logStr);
      HTTPClient patchHttp; patchHttp.begin(firebaseURL); patchHttp.addHeader("Content-Type", "application/json");
      patchHttp.sendRequest("PATCH", logStr); patchHttp.end();
    }

    // --- State Syncing ---
    if(doc.containsKey("tempLow")) { 
      float v = doc["tempLow"]; 
      if(abs(v - (useFahrenheit ? tempLow : (tempLow*9.0/5.0)+32.0)) > 0.5) { tempLow = useFahrenheit ? v : (v-32.0)*5.0/9.0; changed = true; } 
    }
    if(doc.containsKey("tempHigh")) { 
      float v = doc["tempHigh"]; 
      if(abs(v - (useFahrenheit ? tempHigh : (tempHigh*9.0/5.0)+32.0)) > 0.5) { tempHigh = useFahrenheit ? v : (v-32.0)*5.0/9.0; changed = true; } 
    }
    if(doc.containsKey("humLow") && doc["humLow"].as<float>() != humLow) { humLow = doc["humLow"].as<float>(); changed = true; }
    if(doc.containsKey("humHigh") && doc["humHigh"].as<float>() != humHigh) { humHigh = doc["humHigh"].as<float>(); changed = true; }
    if(doc.containsKey("soilLow") && doc["soilLow"].as<float>() != soilLow) { soilLow = doc["soilLow"].as<float>(); changed = true; }
    if(doc.containsKey("soilHigh") && doc["soilHigh"].as<float>() != soilHigh) { soilHigh = doc["soilHigh"].as<float>(); changed = true; }
    if(doc.containsKey("luxThreshold") && doc["luxThreshold"].as<float>() != luxThreshold) { luxThreshold = doc["luxThreshold"].as<float>(); changed = true; }
    
    if(doc.containsKey("timerEnabled") && doc["timerEnabled"].as<bool>() != timerEnabled) { timerEnabled = doc["timerEnabled"].as<bool>(); changed = true; }
    if(doc.containsKey("timeOnHour") && doc["timeOnHour"].as<int>() != timeOnHour) { timeOnHour = doc["timeOnHour"].as<int>(); changed = true; }
    if(doc.containsKey("timeOffHour") && doc["timeOffHour"].as<int>() != timeOffHour) { timeOffHour = doc["timeOffHour"].as<int>(); changed = true; }
    if(doc.containsKey("globalBrightness") && doc["globalBrightness"].as<int>() != globalBrightness) { globalBrightness = doc["globalBrightness"].as<int>(); changed = true; }
    
    if(doc.containsKey("timeZoneOffset") && doc["timeZoneOffset"].as<int>() != timeZoneOffset) { 
      timeZoneOffset = doc["timeZoneOffset"].as<int>(); 
      configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
      sysLog("Timezone updated to UTC " + String(timeZoneOffset));
    }
    if(doc.containsKey("darkMode") && doc["darkMode"].as<bool>() != isDarkMode) { 
      isDarkMode = doc["darkMode"].as<bool>(); changed = true; 
    }

    if(changed) {
      uiNeedsUpdate = true; // Tell Core 1 to redraw sliders!
      sysLog("Cloud settings synced to device.");
    }
  }
  http.end();
}

void checkSerialSensors() {
  while (Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if (data.indexOf("T:") != -1 && data.indexOf("H:") != -1) {
      int tIdx = data.indexOf("T:"), hIdx = data.indexOf("H:"), lIdx = data.indexOf("L:");
      
      float rawTemp = data.substring(tIdx + 2, data.indexOf(',', tIdx)).toFloat();
      liveTemp = useFahrenheit ? rawTemp : (rawTemp - 32.0) * 5.0 / 9.0;
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

void cloudTask(void * parameter) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      
      // 1. Send manual updates from ESP -> Cloud
      if (triggerCloudPush) {
        HTTPClient http; http.begin(firebaseURL); http.addHeader("Content-Type", "application/json");
        StaticJsonDocument<1024> doc;
        doc["tempLow"] = useFahrenheit ? tempLow : (tempLow * 9.0 / 5.0) + 32.0; 
        doc["tempHigh"] = useFahrenheit ? tempHigh : (tempHigh * 9.0 / 5.0) + 32.0; 
        doc["humLow"] = humLow; doc["humHigh"] = humHigh; 
        doc["soilLow"] = soilLow; doc["soilHigh"] = soilHigh; 
        doc["luxThreshold"] = luxThreshold; doc["darkMode"] = isDarkMode;
        String output; serializeJson(doc, output);
        http.sendRequest("PATCH", output); http.end();
        triggerCloudPush = false;
      }

      // 2. Periodic Routine (Every 5 seconds)
      static unsigned long lastPush = 0;
      if (millis() - lastPush > 5000) {
        lastPush = millis();
        
        // Check Cloud for Web Updates (Web -> ESP)
        syncWithCloudSilent(); 
        
        // Push Live Telemetry (ESP -> Web)
        HTTPClient http; http.begin(firebaseURL); http.addHeader("Content-Type", "application/json");
        StaticJsonDocument<512> doc; 
        doc["liveTemp"] = useFahrenheit ? liveTemp : (liveTemp * 9.0 / 5.0) + 32.0; 
        doc["liveHum"] = liveHum; doc["liveLux"] = liveLux;
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

  SPI.begin(TFT_CLK, TFT_MISO, TFT_MOSI);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  ts.begin(SPI);
  ts.setRotation(1);

  lv_init();
  lv_tick_set_cb(millis);

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  build_lvgl_ui();

  WiFi.mode(WIFI_STA);
  int numNetworks = sizeof(myNetworks) / sizeof(myNetworks[0]);
  for (int i = 0; i < numNetworks; i++) {
    WiFi.disconnect(); 
    WiFi.begin(myNetworks[i].ssid, myNetworks[i].pass);
    for (int k = 0; k < 100; k++) { 
      if (WiFi.status() == WL_CONNECTED) break; 
      delay(50); 
    }
    if (WiFi.status() == WL_CONNECTED) break; 
  }
  
  if(WiFi.status() == WL_CONNECTED) sysLog("WiFi Connected to " + WiFi.SSID());

  xTaskCreatePinnedToCore(cloudTask, "CloudTask", 8192, NULL, 1, &CloudTaskHandle, 0);
}

void loop() {
  lv_timer_handler();
  checkSerialSensors();
  
  // This cleanly redraws the LVGL sliders if the background cloud task saw an update!
  if(uiNeedsUpdate) {
    update_ui_from_data();
  }
  
  static unsigned long lastLogic = 0;
  if (millis() - lastLogic > 5000) {
    lastLogic = millis();
    evaluateControlLogic();
  }
  delay(5);
}