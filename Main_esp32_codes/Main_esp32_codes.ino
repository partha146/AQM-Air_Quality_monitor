/*
 * =================================================================================
 * AIR QUALITY MONITOR (AQM) - ESP32 FIRMWARE
 * Created for: Partha A B
 * Microcontroller: ESP32 DevKit V1
 *
 * SENSORS & PINOUT:
 * 1. MTP80-A CO2:   GPIO17 TX2 → Sensor RX, GPIO16 RX2 → Sensor TX
 * 2. PMS7003:       GPIO25 RX1 → Sensor TX, GPIO26 TX1 → Sensor RX
 * 3. BME680:        SDA GPIO21, SCL GPIO22
 * 4. SSD1306 OLED:  SDA GPIO21, SCL GPIO22
 * 5. Push Button:   GPIO4 → GND (INPUT_PULLUP)
 * 6. Active Buzzer: GPIO18 (HIGH = ON)
 *
 * KEY DESIGN: The main loop() is NON-BLOCKING.
 *   - MTP80A uses a state machine (no busy-wait)
 *   - BME680 uses beginReading()/endReading() (no busy-wait)
 *   - Firebase HTTP runs on Core 0 via FreeRTOS (never blocks main loop)
 *   This ensures the button is polled every ~1ms, never missed.
 * =================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME680.h>
#include <time.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Continuous Serial logging at 9600/115200 baud eventually fills the UART
// buffer and makes a "non-blocking" loop block. Leave it off in normal use.
#define AQM_DEBUG 0
#if AQM_DEBUG
#define AQM_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define AQM_LOG(...) \
  do { \
  } while (0)
#endif

// ==========================================
// 1. CONFIGURATION & CREDENTIALS
// ==========================================
Preferences preferences;
String wifi_ssid_1 = "Appu";
String wifi_pass_1 = "appu09875";
String wifi_ssid_2 = "";
String wifi_pass_2 = "";
String wifi_ssid_3 = "";
String wifi_pass_3 = "";
int activeWiFiSlot = 0;
volatile bool needsReconnect = false;  // Set by Core 0 when WiFi creds change

const char FIREBASE_HOST[] = "https://air-quality-monitor-1d94f-default-rtdb.asia-southeast1.firebasedatabase.app";

// Pin Definitions
#define BUTTON_PIN 4
#define BUZZER_PIN 18
#define MTP80A_RX_PIN 16
#define MTP80A_TX_PIN 17
#define PMS_RX_PIN 25
#define PMS_TX_PIN 26

// OLED Setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// BME680
Adafruit_BME680 bme;

// Serial UARTs
HardwareSerial MTP80A_Serial(2);
HardwareSerial PMS_Serial(1);

// MTP80-A Command Frame
const uint8_t MTP80A_CMD_CO2[9] = { 0x42, 0x4D, 0xA0, 0x00, 0x03, 0x00, 0x00, 0x01, 0x32 };

// ==========================================
// 2. SENSOR DATA STRUCTS
// ==========================================
struct SensorData {
  int32_t co2 = -1;
  bool co2Valid = false;
  int pm1_0 = -1;
  int pm2_5 = -1;
  int pm10 = -1;
  bool pmsValid = false;
  float temperature = -999.0;
  float humidity = -1.0;
  float pressure = -1.0;
  float gas_resistance = -1.0;
  float voc_ppm = -1.0;
  bool bmeValid = false;
  int iaq = -1;
  int aqi = -1;
} data;

// Snapshot struct used by Firebase task (avoids data race)
struct FirebaseSnapshot {
  int32_t co2;
  bool co2Valid;
  int pm1_0, pm2_5, pm10;
  float temperature, humidity, pressure, gas_resistance, voc_ppm;
  int iaq, aqi;
};

// ==========================================
// 3. SYSTEM STATE & TIMERS
// ==========================================
int currentPage = 0;
const int TOTAL_PAGES = 5;
volatile bool buzzerEnabled = true;
bool isDangerous = false;
bool buzzerState = false;
bool lastButtonState = HIGH;
unsigned long lastBuzzerToggleTime = 0;
unsigned long lastButtonDebounceTime = 0;
unsigned long lastOLEDUpdateTime = 0;
unsigned long lastFirebaseSyncTime = 0;  // Tracks when Core 0 Firebase task last ran

const unsigned long OLED_UPDATE_INTERVAL = 1000;    // 1s refresh
const unsigned long BUZZER_WARMUP_MS = 20000;       // Silence buzzer for 20s on boot (BME680 heats up)
const unsigned long FIREBASE_SYNC_INTERVAL = 1000;  // live readings: 1 second
const unsigned long HISTORY_SYNC_INTERVAL = 60000;  // history: 1 minute
const unsigned long BUZZER_CONFIG_INTERVAL = 1000;  // mute command: <= 1 second
const unsigned long WIFI_CONFIG_INTERVAL = 5000;    // credentials do not need 1 Hz polling
const unsigned long WIFI_STATUS_INTERVAL = 30000;
const uint16_t FIREBASE_CONNECT_TIMEOUT_MS = 2000;
const uint16_t FIREBASE_READ_TIMEOUT_MS = 3000;
const unsigned long DEBOUNCE_DELAY = 50;  // 50ms is plenty; loop is now fast

// ==========================================
// 4. MTP80A NON-BLOCKING STATE MACHINE
// ==========================================
// Instead of blocking for 600ms waiting for 14 bytes,
// we send the command and pick up bytes as they arrive each loop iteration.
enum MTPState { MTP_IDLE,
                MTP_READING };
MTPState mtpState = MTP_IDLE;
uint8_t mtpBuf[14];
int mtpBufIdx = 0;
unsigned long mtpCmdTime = 0;
unsigned long lastMTPRequest = 0;
const unsigned long MTP_REQUEST_INTERVAL = 2000;  // New CO2 request every 2s (sensor minimum reliable cycle)

uint16_t calcMTPChecksum(const uint8_t* buf, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < len - 2; i++) sum += buf[i];
  return (uint16_t)(sum & 0xFFFF);
}

// Send the CO2 request command, start accumulating response
void mtpSendRequest() {
  while (MTP80A_Serial.available()) MTP80A_Serial.read();  // flush stale
  MTP80A_Serial.write(MTP80A_CMD_CO2, sizeof(MTP80A_CMD_CO2));
  mtpBufIdx = 0;
  mtpCmdTime = millis();
  mtpState = MTP_READING;
}

// Call every loop() — accumulates bytes without blocking
void mtpUpdate() {
  if (mtpState != MTP_READING) return;

  // Grab however many bytes are ready right now
  while (MTP80A_Serial.available() && mtpBufIdx < 14) {
    mtpBuf[mtpBufIdx++] = MTP80A_Serial.read();
  }

  if (mtpBufIdx >= 14) {
    // Full frame received — validate & parse
    mtpState = MTP_IDLE;
    if (mtpBuf[0] != 0x42 || mtpBuf[1] != 0x4D) {
      Serial.println("[MTP80-A] Bad header");
      return;
    }
    uint16_t recv_ck = ((uint16_t)mtpBuf[12] << 8) | mtpBuf[13];
    uint16_t calc_ck = calcMTPChecksum(mtpBuf, 14);
    if (recv_ck != calc_ck) {
      Serial.printf("[MTP80-A] Checksum fail: got %04X exp %04X\n", recv_ck, calc_ck);
      return;
    }
    data.co2 = ((int32_t)mtpBuf[7] << 24) | ((int32_t)mtpBuf[8] << 16) | ((int32_t)mtpBuf[9] << 8) | (int32_t)mtpBuf[10];
    data.co2Valid = (mtpBuf[11] == 0x00);
    AQM_LOG("[MTP80-A] CO2=%d ppm (valid=%s)\n", data.co2, data.co2Valid ? "YES" : "WARMUP");

  } else if (millis() - mtpCmdTime > 700) {
    // Sensor didn't respond in time
    Serial.println("[MTP80-A] Timeout");
    mtpState = MTP_IDLE;
  }
}

// ==========================================
// 5. BME680 NON-BLOCKING
// ==========================================
// Forward declarations (defined in section 7)
float convertGasToVocPpm(float gasKOhms);
int calculateAQI(int pm25);
int calculateIAQ(float gasKOhms, float humidity);

unsigned long bmeReadyAt = 0;
bool bmeReadPending = false;
bool bmeAvailable = false;
unsigned long lastBMERequest = 0;
const unsigned long BME_INTERVAL = 1000;  // New BME reading every 1s (heater cycle is only 150ms)

void bmeStartReading() {
  if (!bmeAvailable) return;
  bmeReadyAt = bme.beginReading();
  if (bmeReadyAt != 0) bmeReadPending = true;
}

void bmeUpdate() {
  if (!bmeReadPending) return;
  if (millis() < bmeReadyAt) return;  // Not ready yet — no block, just return

  if (bme.endReading()) {
    data.temperature = bme.temperature;
    data.humidity = bme.humidity;
    data.pressure = bme.pressure / 100.0F;
    data.gas_resistance = bme.gas_resistance / 1000.0F;
    data.voc_ppm = convertGasToVocPpm(data.gas_resistance);
    data.bmeValid = true;
    bmeReadPending = false;
    AQM_LOG("[BME680] T=%.1f°C H=%.1f%% P=%.1fhPa VOC=%.3fppm\n",
            data.temperature, data.humidity, data.pressure, data.voc_ppm);
  } else {
    // A failed conversion must not leave the state machine permanently busy.
    bmeReadPending = false;
  }
}

// ==========================================
// 6. PMS7003 (already non-blocking)
// ==========================================
uint8_t pmsFrame[32];
uint8_t pmsFrameIndex = 0;

// Byte-wise state machine: it never calls readBytes(), so a partial/corrupt
// PMS frame cannot stall the loop or permanently desynchronise the parser.
void readPMS7003() {
  while (PMS_Serial.available()) {
    const uint8_t value = (uint8_t)PMS_Serial.read();

    if (pmsFrameIndex == 0) {
      if (value == 0x42) pmsFrame[pmsFrameIndex++] = value;
      continue;
    }
    if (pmsFrameIndex == 1) {
      if (value == 0x4D) pmsFrame[pmsFrameIndex++] = value;
      else pmsFrameIndex = (value == 0x42) ? 1 : 0;
      continue;
    }

    pmsFrame[pmsFrameIndex++] = value;
    if (pmsFrameIndex < sizeof(pmsFrame)) continue;

    uint16_t checksum = 0;
    for (uint8_t i = 0; i < 30; ++i) checksum += pmsFrame[i];
    const uint16_t receivedChecksum = ((uint16_t)pmsFrame[30] << 8) | pmsFrame[31];
    const uint16_t frameLength = ((uint16_t)pmsFrame[2] << 8) | pmsFrame[3];

    if (frameLength == 28 && checksum == receivedChecksum) {
      data.pm1_0 = ((int)pmsFrame[10] << 8) | pmsFrame[11];
      data.pm2_5 = ((int)pmsFrame[12] << 8) | pmsFrame[13];
      data.pm10 = ((int)pmsFrame[14] << 8) | pmsFrame[15];
      data.pmsValid = true;
      AQM_LOG("[PMS7003] PM1.0=%d PM2.5=%d PM10=%d ug/m3\n", data.pm1_0, data.pm2_5, data.pm10);
    }
    pmsFrameIndex = 0;
  }
}

// ==========================================
// 7. DERIVED INDICES
// ==========================================
int calculateAQI(int pm25) {
  if (pm25 < 0) return -1;
  if (pm25 <= 12) return map(pm25, 0, 12, 0, 50);
  if (pm25 <= 35) return map(pm25, 13, 35, 51, 100);
  if (pm25 <= 55) return map(pm25, 36, 55, 101, 150);
  if (pm25 <= 150) return map(pm25, 56, 150, 151, 200);
  if (pm25 <= 250) return map(pm25, 151, 250, 201, 300);
  return map(pm25, 251, 500, 301, 500);
}

int calculateIAQ(float gasKOhms, float humidity) {
  if (gasKOhms <= 0 || humidity <= 0) return -1;
  float humScore = abs(humidity - 40.0) * 1.25;
  float gasScore = (gasKOhms < 50.0) ? (50.0 - gasKOhms) * 4.0 : 0.0;
  return constrain((int)(humScore + gasScore + 25.0), 0, 500);
}

float convertGasToVocPpm(float gasKOhms) {
  if (gasKOhms <= 0) return -1.0;
  return constrain((100.0 / gasKOhms) * 0.35, 0.05, 25.0);
}

// ==========================================
// 8. OLED DISPLAY
// ==========================================
void updateOLED() {
  display.clearDisplay();

  // Yellow header bar (top 15px)
  display.fillRect(0, 0, SCREEN_WIDTH, 15, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 4);
  switch (currentPage) {
    case 0: display.print("AIR QUALITY [1/5]"); break;
    case 1: display.print("CO2 & IAQ   [2/5]"); break;
    case 2: display.print("PARTICLES   [3/5]"); break;
    case 3: display.print("CLIMATE     [4/5]"); break;
    case 4: display.print("SYSTEM      [5/5]"); break;
  }

  // Muted indicator "M" in the yellow header bar (right side)
  // Drawn in BLACK here while text color is still BLACK (visible on white bar)
  if (!buzzerEnabled) {
    display.setCursor(108, 4);
    display.print("[M]");
  }

  // Blue body (Y:16-63)
  display.setTextColor(SSD1306_WHITE);

  if (currentPage == 0) {
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("AQI: ");
    display.setTextSize(2);
    if (data.aqi >= 0) display.print(data.aqi);
    else display.print("N/A");
    display.setTextSize(1);
    display.setCursor(0, 42);
    display.print("CO2: ");
    if (data.co2Valid && data.co2 >= 0) {
      display.print(data.co2);
      display.print(" ppm");
    } else display.print("N/A (Warmup)");
    display.setCursor(0, 54);
    display.print("IAQ: ");
    if (data.iaq >= 0) display.print(data.iaq);
    else display.print("N/A");

  } else if (currentPage == 1) {
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("CO2 Level:");
    display.setTextSize(2);
    display.setCursor(0, 30);
    if (data.co2Valid && data.co2 >= 0) {
      display.print(data.co2);
      display.setTextSize(1);
      display.print(" PPM");
    } else display.print("N/A");
    display.setTextSize(1);
    display.setCursor(0, 52);
    display.print("VOC: ");
    if (data.voc_ppm >= 0) {
      display.print(data.voc_ppm, 2);
      display.print(" ppm");
    } else display.print("N/A");

  } else if (currentPage == 2) {
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("PM1.0 : ");
    if (data.pm1_0 >= 0) {
      display.print(data.pm1_0);
      display.print(" ug/m3");
    } else display.print("N/A");
    display.setCursor(0, 35);
    display.print("PM2.5 : ");
    if (data.pm2_5 >= 0) {
      display.print(data.pm2_5);
      display.print(" ug/m3");
    } else display.print("N/A");
    display.setCursor(0, 50);
    display.print("PM10  : ");
    if (data.pm10 >= 0) {
      display.print(data.pm10);
      display.print(" ug/m3");
    } else display.print("N/A");

  } else if (currentPage == 3) {
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Temp : ");
    if (data.temperature > -900) {
      display.print(data.temperature, 1);
      display.print(" C");
    } else display.print("N/A");
    display.setCursor(0, 35);
    display.print("Hum  : ");
    if (data.humidity >= 0) {
      display.print(data.humidity, 1);
      display.print(" %");
    } else display.print("N/A");
    display.setCursor(0, 50);
    display.print("Pres : ");
    if (data.pressure >= 0) {
      display.print(data.pressure, 1);
      display.print(" hPa");
    } else display.print("N/A");

  } else if (currentPage == 4) {
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("WiFi  : ");
    if (WiFi.status() == WL_CONNECTED) {
      display.print("CONNECTED ");
      display.print(activeWiFiSlot);
    } else {
      display.print("DISCONNECTED");
    }
    display.setCursor(0, 35);
    display.print("BUZZER: ");
    display.print(buzzerEnabled ? "ON" : "MUTED");
    display.setCursor(0, 50);
    display.print("Sync  : 1s   OLED: 1s");
  }

  display.display();
}

// ==========================================
// 9. FIREBASE SYNC — RUNS ON CORE 0
// ==========================================
// A one-item queue transfers a complete reading safely between the two cores.
// If HTTP is temporarily slow, xQueueOverwrite keeps the newest reading instead
// of allowing an ever-growing backlog to consume memory.
QueueHandle_t firebaseQueue = nullptr;

bool beginFirebaseRequest(HTTPClient& http, const char* path) {
  char url[192];
  const int length = snprintf(url, sizeof(url), "%s%s", FIREBASE_HOST, path);
  if (length < 0 || length >= (int)sizeof(url)) return false;
  http.setConnectTimeout(FIREBASE_CONNECT_TIMEOUT_MS);
  http.setTimeout(FIREBASE_READ_TIMEOUT_MS);
  return http.begin(url);
}

void checkWiFiConfigUpdate() {
  HTTPClient http;
  if (!beginFirebaseRequest(http, "/config/wifi.json")) return;
  if (http.GET() == 200) {
    String payload = http.getString();

    // Simple JSON key extractor for string values
    auto extract = [&](String key) -> String {
      String search = "\"" + key + "\":\"";
      int idx = payload.indexOf(search);
      if (idx == -1) return "";
      idx += search.length();
      int endIdx = payload.indexOf("\"", idx);
      if (endIdx == -1) return "";
      return payload.substring(idx, endIdx);
    };

    String s1 = extract("ssid1");
    String p1 = extract("pass1");
    String s2 = extract("ssid2");
    String p2 = extract("pass2");
    String s3 = extract("ssid3");
    String p3 = extract("pass3");

    // Only act if we got something meaningful AND something changed
    bool changed = (s1.length() > 0) && (s1 != wifi_ssid_1 || p1 != wifi_pass_1 || s2 != wifi_ssid_2 || p2 != wifi_pass_2 || s3 != wifi_ssid_3 || p3 != wifi_pass_3);

    if (changed) {
      wifi_ssid_1 = s1;
      wifi_pass_1 = p1;
      wifi_ssid_2 = s2;
      wifi_pass_2 = p2;
      wifi_ssid_3 = s3;
      wifi_pass_3 = p3;

      // Persist to NVS flash
      preferences.begin("aqm", false);
      preferences.putString("ssid1", s1);
      preferences.putString("pass1", p1);
      preferences.putString("ssid2", s2);
      preferences.putString("pass2", p2);
      preferences.putString("ssid3", s3);
      preferences.putString("pass3", p3);
      preferences.end();

      Serial.println("[WiFi] Config changed — will reconnect with new credentials.");
      needsReconnect = true;  // Trigger reconnect on next loop iteration in Core 0
    }
  }
  http.end();
}

void buildJsonPayload(const FirebaseSnapshot& s, char* json, size_t jsonSize) {
  // Avoid repeated Arduino String concatenation in the 1 Hz path. Heap
  // fragmentation is a common cause of ESP32s becoming less reliable over time.
  const time_t now = time(nullptr);
  const unsigned long long timestamp = (now > 1000000000)
                                         ? (unsigned long long)now * 1000ULL
                                         : (unsigned long long)millis();

  snprintf(json, jsonSize,
           "{\"co2\":%ld,\"aqi\":%d,\"iaq\":%d,\"pm1_0\":%d,\"pm2_5\":%d,\"pm10\":%d,"
           "\"temp\":%.1f,\"hum\":%.1f,\"pres\":%.1f,\"gas\":%.1f,\"voc_ppm\":%.2f,\"timestamp\":%llu}",
           (long)(s.co2Valid ? s.co2 : -1), s.aqi, s.iaq, s.pm1_0, s.pm2_5, s.pm10,
           s.temperature > -900 ? s.temperature : 0.0F,
           s.humidity >= 0 ? s.humidity : 0.0F,
           s.pressure >= 0 ? s.pressure : 0.0F,
           s.gas_resistance >= 0 ? s.gas_resistance : 0.0F,
           s.voc_ppm >= 0 ? s.voc_ppm : 0.0F,
           timestamp);
}

// Publish the active WiFi status so the app can read it
void publishActiveWiFiStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  String ssids[3] = { wifi_ssid_1, wifi_ssid_2, wifi_ssid_3 };
  String activeSSID = (activeWiFiSlot >= 1 && activeWiFiSlot <= 3)
                        ? ssids[activeWiFiSlot - 1]
                        : "";

  String json = "{\"activeSlot\":" + String(activeWiFiSlot) + ",\"activeSSID\":\"" + activeSSID + "\"}";

  HTTPClient http;
  if (!beginFirebaseRequest(http, "/status/wifi.json")) return;
  http.addHeader("Content-Type", "application/json");
  http.PUT(json);
  http.end();
  AQM_LOG("[Firebase] WiFi status published: Slot %d (%s)\n",
          activeWiFiSlot, activeSSID.c_str());
}

// This task runs on Core 0. Main loop (Core 1) is never blocked by HTTP.
void firebaseSyncTask(void* pvParam) {
  unsigned long lastBuzzerConfigCheck = 0;
  unsigned long lastWiFiConfigCheck = 0;
  unsigned long lastHistorySync = 0;
  unsigned long lastWiFiStatusSync = 0;
  unsigned long lastReconnectAt = 0;               // Cooldown: don't spam reconnects
  const unsigned long RECONNECT_COOLDOWN = 30000;  // 30s between reconnect attempts

  for (;;) {
    // Handle reconnect with cooldown to avoid spamming WiFi.disconnect/begin
    bool wifiDown = (WiFi.status() != WL_CONNECTED);
    if (needsReconnect || wifiDown) {
      unsigned long now = millis();
      // First attempt or cooldown expired
      if (needsReconnect || (now - lastReconnectAt >= RECONNECT_COOLDOWN)) {
        if (wifiDown) Serial.println("[WiFi] Connection lost — reconnecting...");
        if (needsReconnect) Serial.println("[WiFi] Credential change — reconnecting...");
        lastReconnectAt = now;
        connectMultiWiFi();
        // Publish status after reconnect attempt (connected or not)
        if (WiFi.status() == WL_CONNECTED) publishActiveWiFiStatus();
      } else {
        // Cooldown active — just wait
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
      continue;
    }

    const unsigned long now = millis();

    // Poll the mute command every second. A missing Firebase value does not
    // overwrite the safe default, so a fresh database keeps the buzzer enabled.
    if (now - lastBuzzerConfigCheck >= BUZZER_CONFIG_INTERVAL) {
      lastBuzzerConfigCheck = now;

      HTTPClient http;
      if (beginFirebaseRequest(http, "/config/buzzerEnabled.json") && http.GET() == 200) {
        String p = http.getString();
        p.trim();
        if (p == "true") buzzerEnabled = true;
        else if (p == "false") buzzerEnabled = false;
      }
      http.end();
    }

    if (now - lastWiFiConfigCheck >= WIFI_CONFIG_INTERVAL) {
      lastWiFiConfigCheck = now;
      checkWiFiConfigUpdate();
    }

    FirebaseSnapshot s;
    if (xQueueReceive(firebaseQueue, &s, 0) == pdPASS) {
      char json[256];
      buildJsonPayload(s, json, sizeof(json));
      HTTPClient http;
      if (beginFirebaseRequest(http, "/readings/current.json")) {
        http.addHeader("Content-Type", "application/json");
        const int putCode = http.PUT((uint8_t*)json, strlen(json));
        AQM_LOG("[Firebase] Live upload: HTTP %d\n", putCode);
        http.end();
      }

      // Preserve useful charts without creating 86,400 history records/day.
      if (now - lastHistorySync >= HISTORY_SYNC_INTERVAL) {
        lastHistorySync = now;
        if (beginFirebaseRequest(http, "/history.json")) {
          http.addHeader("Content-Type", "application/json");
          http.POST((uint8_t*)json, strlen(json));
          http.end();
        }
      }
    }

    if (now - lastWiFiStatusSync >= WIFI_STATUS_INTERVAL) {
      lastWiFiStatusSync = now;
      publishActiveWiFiStatus();
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// ==========================================
// 10. SETUP
// ==========================================
void loadWiFiConfig() {
  preferences.begin("aqm", false);
  // getString returns default if key doesn't exist — no isKey() needed
  String s1 = preferences.getString("ssid1", "");
  // If NVS is empty (fresh flash), use hardcoded defaults and save them
  if (s1.length() == 0) {
    wifi_ssid_1 = "Appu";
    wifi_pass_1 = "appu09875";
    wifi_ssid_2 = "";
    wifi_pass_2 = "";
    wifi_ssid_3 = "";
    wifi_pass_3 = "";
    preferences.putString("ssid1", wifi_ssid_1);
    preferences.putString("pass1", wifi_pass_1);
    preferences.putString("ssid2", wifi_ssid_2);
    preferences.putString("pass2", wifi_pass_2);
    preferences.putString("ssid3", wifi_ssid_3);
    preferences.putString("pass3", wifi_pass_3);
    Serial.println("[WiFi] NVS empty — defaults written (Appu).");
  } else {
    wifi_ssid_1 = s1;
    wifi_pass_1 = preferences.getString("pass1", "appu09875");
    wifi_ssid_2 = preferences.getString("ssid2", "");
    wifi_pass_2 = preferences.getString("pass2", "");
    wifi_ssid_3 = preferences.getString("ssid3", "");
    wifi_pass_3 = preferences.getString("pass3", "");
  }
  Serial.printf("[WiFi] Loaded: Slot1='%s' Slot2='%s' Slot3='%s'\n",
                wifi_ssid_1.c_str(), wifi_ssid_2.c_str(), wifi_ssid_3.c_str());
  preferences.end();
}

void connectMultiWiFi() {
  String ssids[3] = { wifi_ssid_1, wifi_ssid_2, wifi_ssid_3 };
  String passes[3] = { wifi_pass_1, wifi_pass_2, wifi_pass_3 };

  WiFi.disconnect(false);  // drop current connection, keep radio alive
  vTaskDelay(pdMS_TO_TICKS(200));

  for (int i = 0; i < 3; i++) {
    if (ssids[i].length() == 0) continue;

    Serial.printf("[WiFi] Trying Slot %d: '%s'...\n", i + 1, ssids[i].c_str());
    WiFi.begin(ssids[i].c_str(), passes[i].c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {  // 10s per slot
      vTaskDelay(pdMS_TO_TICKS(500));
      Serial.print(".");
      retry++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      activeWiFiSlot = i + 1;
      needsReconnect = false;
      Serial.printf("[WiFi] Connected on Slot %d (%s)  IP: %s\n",
                    activeWiFiSlot, ssids[i].c_str(), WiFi.localIP().toString().c_str());
      return;
    }

    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  // HARDCODED FALLBACK — always try Appu if all configured slots failed
  // ↓↓ CHANGE HERE if your permanent home WiFi ever changes ↓↓
  Serial.println("[WiFi] Configured slots failed. Trying hardcoded fallback: Appu");
  WiFi.begin("Appu", "appu09875");  // ← CHANGE HERE (Ctrl+F 'CHANGE HERE')
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    retry++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    activeWiFiSlot = 1;
    needsReconnect = false;
    // Reset NVS so next boot doesn't try bad credentials again
    preferences.begin("aqm", false);
    preferences.putString("ssid1", "Appu");
    preferences.putString("pass1", "appu09875");
    preferences.putString("ssid2", "");
    preferences.putString("pass2", "");
    preferences.putString("ssid3", "");
    preferences.putString("pass3", "");
    preferences.end();
    wifi_ssid_1 = "Appu";
    wifi_pass_1 = "appu09875";  // ← CHANGE HERE
    wifi_ssid_2 = "";
    wifi_pass_2 = "";
    wifi_ssid_3 = "";
    wifi_pass_3 = "";
    Serial.printf("[WiFi] Fallback connected  IP: %s  NVS reset to Appu.\n",
                  WiFi.localIP().toString().c_str());
    return;
  }

  Serial.println("[WiFi] All slots failed. Will retry in 30s.");
  activeWiFiSlot = 0;
  needsReconnect = false;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=============================================");
  Serial.println("   AIR QUALITY MONITOR (AQM) - Non-blocking");
  Serial.println("=============================================");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // UART sensors
  MTP80A_Serial.begin(9600, SERIAL_8N1, MTP80A_RX_PIN, MTP80A_TX_PIN);
  PMS_Serial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

  // I2C: OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[Init] SSD1306 failed!");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 20);
    display.println("AQM SYSTEM INIT");
    display.setCursor(20, 38);
    display.println("by Partha A B");
    display.display();
    delay(2500);  // reduced from 10s via wifi retry loops
  }

  // BME680
  if (!bme.begin(0x77) && !bme.begin(0x76)) {
    Serial.println("[Init] BME680 not found!");
  } else {
    bmeAvailable = true;
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    Serial.println("[Init] BME680 OK.");
  }

  // WiFi — boot-time connect (use delay(), FreeRTOS not running yet)
  loadWiFiConfig();
  {
    String ssids[3] = { wifi_ssid_1, wifi_ssid_2, wifi_ssid_3 };
    String passes[3] = { wifi_pass_1, wifi_pass_2, wifi_pass_3 };
    // Disable ESP32's built-in WiFi persistence so it doesn't auto-connect
    // to a stale saved network and interfere with our own slot logic
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    delay(100);
    for (int i = 0; i < 3; i++) {
      if (ssids[i].length() == 0) continue;
      Serial.printf("[WiFi] Trying Slot %d: '%s'...\n", i + 1, ssids[i].c_str());
      WiFi.begin(ssids[i].c_str(), passes[i].c_str());
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {  // 10s per slot
        delay(500);
        Serial.print(".");
        retry++;
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        activeWiFiSlot = i + 1;
        Serial.printf("[WiFi] Connected on Slot %d  IP: %s\n",
                      activeWiFiSlot, WiFi.localIP().toString().c_str());
        break;
      }
      WiFi.disconnect(false);
      delay(300);
    }
    // HARDCODED FALLBACK — always try Appu if all configured slots failed
    // This guarantees the device can always come home even if Firebase has bad data
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Configured slots failed. Trying hardcoded fallback: Appu");
      WiFi.begin("Appu", "appu09875");  // ← CHANGE HERE (Ctrl+F 'CHANGE HERE')
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        activeWiFiSlot = 1;
        // Also reset NVS and Firebase to correct defaults
        preferences.begin("aqm", false);
        preferences.putString("ssid1", "Appu");
        preferences.putString("pass1", "appu09875");  // ← CHANGE HERE
        preferences.putString("ssid2", "");
        preferences.putString("pass2", "");
        preferences.putString("ssid3", "");
        preferences.putString("pass3", "");
        preferences.end();
        wifi_ssid_1 = "Appu";
        wifi_pass_1 = "appu09875";  // ← CHANGE HERE
        wifi_ssid_2 = "";
        wifi_pass_2 = "";
        wifi_ssid_3 = "";
        wifi_pass_3 = "";
        Serial.printf("[WiFi] Fallback connected  IP: %s  NVS reset to Appu.\n",
                      WiFi.localIP().toString().c_str());
      }
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Failed to connect on any slot.");
      activeWiFiSlot = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    // NTP sync (IST = UTC+5:30 = 19800s)
    configTime(19800, 0, "pool.ntp.org", "time.google.com");
    struct tm ti;
    int ntpTry = 0;
    while (!getLocalTime(&ti) && ntpTry < 10) {
      delay(500);
      ntpTry++;
    }
    if (ntpTry < 10)
      Serial.printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d IST\n",
                    ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
    else
      Serial.println("[NTP] Timeout — will use millis() as timestamp.");

    // Push current WiFi config + status to Firebase so the app reads real values
    {
      HTTPClient http;
      String wifiJson = "{\"ssid1\":\"" + wifi_ssid_1 + "\",\"pass1\":\"" + wifi_pass_1 + "\","
                                                                                          "\"ssid2\":\""
                        + wifi_ssid_2 + "\",\"pass2\":\"" + wifi_pass_2 + "\","
                                                                          "\"ssid3\":\""
                        + wifi_ssid_3 + "\",\"pass3\":\"" + wifi_pass_3 + "\"}";
      if (beginFirebaseRequest(http, "/config/wifi.json")) {
        http.addHeader("Content-Type", "application/json");
        // Only PUT if Firebase has nothing yet — use GET first to check.
        const bool configMissing = (http.GET() != 200 || http.getString().length() < 10);
        http.end();
        if (configMissing && beginFirebaseRequest(http, "/config/wifi.json")) {
          http.addHeader("Content-Type", "application/json");
          http.PUT(wifiJson);
          http.end();
          Serial.println("[Firebase] Default WiFi config pushed to Firebase.");
        }
      }

      // Always publish active WiFi status
      String statusJson = "{\"activeSlot\":" + String(activeWiFiSlot) + ",\"activeSSID\":\"" + wifi_ssid_1 + "\"}";
      if (beginFirebaseRequest(http, "/status/wifi.json")) {
        http.addHeader("Content-Type", "application/json");
        http.PUT(statusJson);
        http.end();
      }
    }
  } else {
    Serial.println("\n[WiFi] Timeout.");
  }

  // Launch Firebase sync task on Core 0 (16KB stack for HTTP).
  firebaseQueue = xQueueCreate(1, sizeof(FirebaseSnapshot));
  if (firebaseQueue == nullptr) {
    Serial.println("[FreeRTOS] Firebase queue allocation failed.");
  } else {
    xTaskCreatePinnedToCore(firebaseSyncTask, "FirebaseTask", 16384, NULL, 1, NULL, 0);
    Serial.println("[FreeRTOS] Firebase task pinned to Core 0.");
  }

  updateOLED();
  Serial.println("[Init] Setup complete. Entering non-blocking main loop.");
}

// ==========================================
// 11. MAIN LOOP — TIGHT, NON-BLOCKING
// ==========================================
void loop() {
  unsigned long now = millis();

  // ── A. BUTTON (edge-triggered, always polled first) ──
  bool btnState = digitalRead(BUTTON_PIN);
  if (btnState == LOW && lastButtonState == HIGH) {
    if (now - lastButtonDebounceTime > DEBOUNCE_DELAY) {
      currentPage = (currentPage + 1) % TOTAL_PAGES;
      lastButtonDebounceTime = now;
      Serial.printf("[UI] Page → %d/%d\n", currentPage + 1, TOTAL_PAGES);
      updateOLED();  // Immediate display update on button press
    }
  }
  lastButtonState = btnState;

  // ── B. MTP80A — send request every 3s, accumulate bytes non-blocking ──
  if (mtpState == MTP_IDLE && now - lastMTPRequest >= MTP_REQUEST_INTERVAL) {
    lastMTPRequest = now;
    mtpSendRequest();
  }
  mtpUpdate();  // Always called — picks up any available bytes instantly

  // ── C. PMS7003 — non-blocking, reads if bytes are available ──
  readPMS7003();

  // ── D. BME680 — non-blocking start/end reading ──
  if (!bmeReadPending && now - lastBMERequest >= BME_INTERVAL) {
    lastBMERequest = now;
    bmeStartReading();
  }
  bmeUpdate();  // Returns instantly if not yet ready

  // ── E. Derived indices ──
  data.aqi = calculateAQI(data.pm2_5);
  data.iaq = calculateIAQ(data.gas_resistance, data.humidity);

  // ── F. Danger assessment ──
  isDangerous = ((data.co2Valid && data.co2 > 1000) || (data.pm2_5 > 35) || (data.aqi > 100) || (data.iaq > 150));

  // ── G. OLED periodic refresh (not on button press — that's immediate above) ──
  if (now - lastOLEDUpdateTime >= OLED_UPDATE_INTERVAL) {
    lastOLEDUpdateTime = now;
    updateOLED();
  }

  // ── H. Buzzer ──
  // Suppress buzzer for first 20s: BME680 MOX coil is heating up and gives
  // spuriously high gas resistance (very high VOC/IAQ) until it stabilises.
  bool buzzerAllowed = (now >= BUZZER_WARMUP_MS);
  if (isDangerous && buzzerEnabled && buzzerAllowed) {
    if (now - lastBuzzerToggleTime >= 500) {
      lastBuzzerToggleTime = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }

  // ── I. Hand off the newest reading to Core 0 (instant, no HTTP here) ──
  if (now - lastFirebaseSyncTime >= FIREBASE_SYNC_INTERVAL) {
    lastFirebaseSyncTime = now;
    if (firebaseQueue != nullptr) {
      FirebaseSnapshot snapshot = {
        data.co2, data.co2Valid,
        data.pm1_0, data.pm2_5, data.pm10,
        data.temperature, data.humidity, data.pressure,
        data.gas_resistance, data.voc_ppm,
        data.iaq, data.aqi
      };
      xQueueOverwrite(firebaseQueue, &snapshot);
    }
  }
  // No delay() here — loop runs as fast as possible (~microseconds per iteration)
}
