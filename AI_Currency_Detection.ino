#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "board_config.h"

// ================= WIFI =================
const char* ssid     = "moto";
const char* password = "00000000";

// ================= SERVER =================
const char* server = "http://10.166.121.244:5000/detect";

// ================= OLED =================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= GLOBALS =================
HTTPClient http;          // persistent across frames — avoids TCP reconnect overhead
bool       httpReady = false;
String     lastResult = "";
uint32_t   frameCount = 0;
uint32_t   lastFpsTime = 0;

// ================= OLED HELPER =================
void showOLED(const String& line1, const String& line2 = "") {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.println(line1);
  if (line2.length()) {
    display.setTextSize(1);
    display.setCursor(0, 40);
    display.println(line2);
  }
  display.display();
}

// ================= SEND IMAGE =================
void sendImage() {
  // --- Flush stale frame from buffer so we get the freshest one ---
  camera_fb_t* stale = esp_camera_fb_get();
  if (stale) esp_camera_fb_return(stale);

  // --- Capture fresh frame (no flash delay needed in lit environments) ---
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERR] Capture failed");
    showOLED("CAP ERR");
    return;
  }

  Serial.printf("[INFO] Frame size: %u bytes\n", fb->len);

  // --- Reuse persistent HTTP connection ---
  if (!httpReady || !http.connected()) {
    http.end();  // clean slate if stale
    http.begin(server);
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(5000);   // tight timeout — fail fast, retry next frame
    http.setReuse(true);
    httpReady = true;
    Serial.println("[INFO] HTTP reconnected");
  }

  int code = http.POST(fb->buf, fb->len);

  // Release camera buffer IMMEDIATELY after POST — don't hold it during response parsing
  esp_camera_fb_return(fb);

  if (code == 200) {
    String result = http.getString();
    result.trim();

    Serial.printf("[DET] %s\n", result.c_str());

    frameCount++;
    uint32_t now = millis();
    float fps = 0;
    if (now - lastFpsTime >= 1000) {
      fps = frameCount * 1000.0f / (now - lastFpsTime);
      frameCount = 0;
      lastFpsTime = now;
    }

    // Only redraw OLED if result changed — saves ~5ms I2C write per frame
    if (result != lastResult) {
      char fpsStr[16];
      snprintf(fpsStr, sizeof(fpsStr), "FPS:%.1f", fps);
      showOLED(result, String(fpsStr));
      lastResult = result;
    }

  } else {
    Serial.printf("[ERR] HTTP %d\n", code);
    showOLED("HTTP ERR", String(code));
    // Force reconnect next frame
    http.end();
    httpReady = false;
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== BOOT ===");

  // --- OLED ---
  Wire.begin(13, 14);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERR] OLED init failed");
    while (1);
  }
  display.setTextColor(WHITE);
  showOLED("Starting..");

  // --- Camera config ---
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // QQVGA (160x120) — YOLO resizes to 320 anyway, half the WiFi payload
  config.frame_size   = FRAMESIZE_QQVGA;
  config.jpeg_quality = 12;   // 10–15 sweet spot: small size, acceptable quality

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count    = 2;   // double buffer: one capturing while one is in flight
    Serial.println("[INFO] PSRAM found, using 2 frame buffers");
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
    Serial.println("[WARN] No PSRAM — single buffer mode");
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("[ERR] Camera init failed");
    showOLED("CAM FAIL");
    while (1);
  }

  // Lock sensor settings so they don't drift frame-to-frame
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QQVGA);
  s->set_quality(s, 12);
  s->set_whitebal(s, 1);       // auto white balance on
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);  // auto exposure on
  s->set_aec2(s, 1);           // improved AEC
  s->set_gainceiling(s, (gainceiling_t)2);

  // Flash pin
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);

  // --- WiFi ---
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);         // disable modem sleep — keeps latency low
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // max TX power for stability

  showOLED("WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\n[INFO] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  showOLED("Ready!", WiFi.localIP().toString());
  delay(800);

  lastFpsTime = millis();
}

// ================= LOOP =================
void loop() {
  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi lost, reconnecting...");
    showOLED("WiFi lost");
    WiFi.reconnect();
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) delay(300);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[ERR] WiFi reconnect failed");
      return;
    }
    Serial.println("[INFO] WiFi restored");
    httpReady = false;  // force HTTP reconnect too
  }

  sendImage();
  // NO delay() — HTTP round-trip is the natural throttle
}
