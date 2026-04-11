#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

// ===========================
// 1. Credentials & Cloud
// ===========================
//const char* ssid = "MASCU-Fi";
//const char* password = "milliondollarbackyard@1192";
//const char* ssid = "shanel";
//const char* password = "jamescharles";
const char* ssid = "ASUS";
const char* password = "smartspark701";
// Removed trailing slash for cleaner URL building
const char* FIREBASE_DB_URL = "https://plant-enclosure-default-rtdb.firebaseio.com";

// ===========================
// 2. Camera Pin Mapping
// ===========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_LED_PIN      4

httpd_handle_t plant_httpd = NULL;

// ===========================
// 3. Web Server Handlers
// ===========================

static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t flash_handler(httpd_req_t *req) {
    char buf[20];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "v", param, sizeof(param)) == ESP_OK) {
            int val = atoi(param);
            if (val > 255) val = 255;
            if (val < 0) val = 0;
            // UPDATED: New API uses the PIN number instead of a channel
            ledcWrite(FLASH_LED_PIN, val); 
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// --- NEW: Camera Action Handler for Filters & Settings ---
static esp_err_t cmd_handler(httpd_req_t *req) {
    char buf[64];
    char variable[32];
    char value[32];

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
            httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            
            int val = atoi(value);
            sensor_t * s = esp_camera_sensor_get();
            int res = 0;

            if(!strcmp(variable, "framesize")) {
                if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
            }
            else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
            else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
            else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
            else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
            else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
            else if(!strcmp(variable, "special_effect")) res = s->set_special_effect(s, val);
            else res = -1;

            if(res) {
                return httpd_resp_send_500(req);
            }
            
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    return httpd_resp_send_404(req);
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char * part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if(res != ESP_OK) return res;

    while(true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            res = ESP_FAIL;
        } else {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
            if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
            if(res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            esp_camera_fb_return(fb);
        }
        if(res != ESP_OK) break;
    }
    return res;
}

void startServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };
    httpd_uri_t flash_uri   = { .uri = "/flash",   .method = HTTP_GET, .handler = flash_handler,   .user_ctx = NULL };
    httpd_uri_t cmd_uri     = { .uri = "/action",  .method = HTTP_GET, .handler = cmd_handler,     .user_ctx = NULL }; // NEW ENDPOINT

    if (httpd_start(&plant_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(plant_httpd, &capture_uri);
        httpd_register_uri_handler(plant_httpd, &stream_uri);
        httpd_register_uri_handler(plant_httpd, &flash_uri);
        httpd_register_uri_handler(plant_httpd, &cmd_uri); // REGISTER NEW ENDPOINT
    }
}

void pushIpToFirebase() {
    HTTPClient http;
    String url = String(FIREBASE_DB_URL) + "/settings.json";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"camera_ip\":\"http://" + WiFi.localIP().toString() + "\"}";
    int responseCode = http.PATCH(payload);
    
    if (responseCode > 0) {
        Serial.println("IP Uploaded to Firebase successfully!");
    } else {
        Serial.printf("Firebase Upload Failed, Error: %d\n", responseCode);
    }
    http.end();
}

void setup() {
  // 1. ABSOLUTE FIRST LINE: Kill the safety switch
  // This prevents the ESP32 from resetting if the voltage dips for a microsecond
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 

  // 2. POWER STABILIZATION DELAY
  // Essential for 9V batteries and DC-DC converters. 
  // This gives the converter time to fully "charge" the circuit before we draw heavy current.
  delay(2000); 

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n[SYSTEM] Power stabilized. Starting boot sequence...");

  // 3. REPAIR INTERNAL MEMORY (NVS)
  // Fixes the "ret=ffffffff" error caused by previous power crashes
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 4. SETUP FLASH LED (New Version 3.0 Syntax)
  // We attach the pin first but keep it OFF to save battery juice
  ledcAttach(FLASH_LED_PIN, 5000, 8); 
  ledcWrite(FLASH_LED_PIN, 0); 

  // 5. CAMERA CONFIGURATION
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Use lower resolution on boot to minimize the initial power spike
  config.frame_size = FRAMESIZE_VGA; 
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // Initialize the Camera "Eye"
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera failed 0x%x", err);
    delay(5000);
    ESP.restart(); // If the camera fails, try a fresh reboot
    return;
  }

  // 6. CONNECT WIFI (The heaviest power draw)
  WiFi.mode(WIFI_STA);
  vTaskDelay(10); 
  WiFi.setTxPower(WIFI_POWER_7dBm); // Set to a lower power level

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  // Use a timeout to prevent infinite loops if battery dies
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500);
    Serial.print(".");
    timeout++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    
    // 7. START WEB SERVER
    startServer();
    
    Serial.print("Camera Ready! IP: ");
    Serial.println(WiFi.localIP());

    // 8. SYNC IP TO FIREBASE
    // This lets control.html find the camera automatically
    pushIpToFirebase();
  } else {
    Serial.println("\nWiFi Failed. Checking battery power...");
  }
}

void loop() {
    delay(1000);
}