#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <DallasTemperature.h>
#include <OneWire.h>

namespace {

constexpr char WIFI_SSID[] = "Fablab";
constexpr char WIFI_PASSWORD[] = "fablabshanghai";

constexpr char THINGSPEAK_WRITE_API_KEY[] = "03RHR3I8B58K72E3";

constexpr char CONFIG_AP_PASSWORD[] = "ecofish123";
constexpr uint8_t CONFIG_AP_CHANNEL = 1;
constexpr bool CONFIG_AP_HIDDEN = false;
constexpr uint8_t CONFIG_AP_MAX_CONN = 4;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

constexpr bool THINGSPEAK_USE_HTTPS = false;
constexpr uint32_t THINGSPEAK_HTTP_TIMEOUT_MS = 4000;

constexpr int PIN_USER_BUTTON = 27;
constexpr bool USER_BUTTON_ACTIVE_LOW = true;
constexpr uint32_t USER_BUTTON_DEBOUNCE_MS = 40;

constexpr int PIN_I2C_SDA = 21;
constexpr int PIN_I2C_SCL = 22;

constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
constexpr int OLED_RESET_PIN = -1;
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;

constexpr int PIN_TDS_ADC = 34;
constexpr int PIN_LIGHT_ADC = 35;
constexpr int PIN_ULTRASONIC_TRIG = 25;
constexpr int PIN_ULTRASONIC_ECHO = 26;
constexpr int PIN_DS18B20_DATA = 4;

constexpr float ADC_REFERENCE_VOLTAGE = 3.30f;
constexpr int ADC_MAX_VALUE = 4095;

constexpr float TDS_K_VALUE = 1.0f;

constexpr int LIGHT_ADC_MIN = 0;
constexpr int LIGHT_ADC_MAX = 4095;

constexpr float TANK_DEPTH_CM = 30.0f;

constexpr uint32_t SENSOR_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t THINGSPEAK_UPDATE_INTERVAL_MS = 20000;

OneWire oneWire(PIN_DS18B20_DATA);
DallasTemperature ds18b20(&oneWire);

Preferences preferences;
WebServer webServer(80);
DNSServer dnsServer;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
bool displayOk = false;

float lastTemperatureC = NAN;
float lastTdsPpm = NAN;
float lastWaterLevelCm = NAN;
float lastLightPercent = NAN;
bool lastThingSpeakOk = false;

bool portalActive = false;
String portalApSsid;

SemaphoreHandle_t portalMutex = nullptr;
TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t portalTaskHandle = nullptr;
TaskHandle_t networkTaskHandle = nullptr;
TaskHandle_t buttonTaskHandle = nullptr;

portMUX_TYPE readingsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE settingsMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t thingSpeakIntervalMs = THINGSPEAK_UPDATE_INTERVAL_MS;

volatile bool wifiReconnectRequested = false;
volatile bool portalButtonRequested = false;
volatile bool forcePortalMode = false;

uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

String chipSuffix() {
  const uint64_t mac = ESP.getEfuseMac();
  char buf[7] = {0};
  const uint32_t low24 = static_cast<uint32_t>(mac & 0xFFFFFFULL);
  snprintf(buf, sizeof(buf), "%06X", low24);
  return String(buf);
}

bool isPlaceholder(const char* s) {
  if (s == nullptr) return true;
  return strstr(s, "YOUR_") != nullptr;
}

void loadWiFiCredentials(String& ssid, String& pass) {
  ssid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  pass = preferences.isKey("pass") ? preferences.getString("pass", "") : "";

  if (ssid.length() == 0 && !isPlaceholder(WIFI_SSID)) {
    ssid = WIFI_SSID;
    pass = WIFI_PASSWORD;
  }
}

void saveWiFiCredentials(const String& ssid, const String& pass) {
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
}

uint32_t parseU32OrZero(const String& s) {
  if (s.length() == 0) return 0;
  const long v = s.toInt();
  if (v <= 0) return 0;
  return static_cast<uint32_t>(v);
}

void loadThingSpeakIntervalHms(uint32_t& hours, uint32_t& minutes, uint32_t& seconds) {
  hours = preferences.isKey("ts_h") ? preferences.getUInt("ts_h", 0) : 0;
  minutes = preferences.isKey("ts_m") ? preferences.getUInt("ts_m", 0) : 0;
  seconds = preferences.isKey("ts_s") ? preferences.getUInt("ts_s", 0) : 0;
  minutes = clampU32(minutes, 0, 59);
  seconds = clampU32(seconds, 0, 59);
}

void saveThingSpeakIntervalHms(uint32_t hours, uint32_t minutes, uint32_t seconds) {
  minutes = clampU32(minutes, 0, 59);
  seconds = clampU32(seconds, 0, 59);
  preferences.putUInt("ts_h", hours);
  preferences.putUInt("ts_m", minutes);
  preferences.putUInt("ts_s", seconds);
}

uint32_t computeThingSpeakIntervalMsFromPrefs() {
  uint32_t h = 0;
  uint32_t m = 0;
  uint32_t s = 0;
  loadThingSpeakIntervalHms(h, m, s);
  const uint32_t totalSeconds = h * 3600U + m * 60U + s;
  if (totalSeconds == 0) return THINGSPEAK_UPDATE_INTERVAL_MS;
  const uint64_t ms = static_cast<uint64_t>(totalSeconds) * 1000ULL;
  if (ms > 0xFFFFFFFFULL) return 0xFFFFFFFFUL;
  return static_cast<uint32_t>(ms);
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String buildNetworkOptionsHtml(const String& selectedSsid) {
  String options;
  const int n = WiFi.scanComplete();
  if (n <= 0) {
    WiFi.scanNetworks(true);
    return options;
  }

  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    options += "<option value=\"";
    options += htmlEscape(ssid);
    options += "\"";
    if (ssid == selectedSsid) {
      options += " selected";
    }
    options += ">";
    options += htmlEscape(ssid);
    options += "</option>";
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  return options;
}

void stopConfigPortal() {
  if (!portalActive) return;
  if (portalMutex != nullptr) {
    xSemaphoreTakeRecursive(portalMutex, portMAX_DELAY);
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  portalApSsid = "";
  if (portalMutex != nullptr) {
    xSemaphoreGiveRecursive(portalMutex);
  }
}

void handlePortalRoot() {
  String savedSsid;
  String savedPass;
  uint32_t savedTsH = 0;
  uint32_t savedTsM = 0;
  uint32_t savedTsS = 0;
  loadWiFiCredentials(savedSsid, savedPass);
  loadThingSpeakIntervalHms(savedTsH, savedTsM, savedTsS);

  String html;
  html.reserve(3072);
  html += "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>EcoFishTank WiFi</title></head><body>";
  html += "<h2>EcoFishTank WiFi 配置</h2>";
  html += "<p>AP: ";
  html += htmlEscape(portalApSsid);
  html += "</p>";
  html += "<p>当前状态: ";
  html += (WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
  html += "</p>";
  if (WiFi.status() == WL_CONNECTED) {
    html += "<p>STA IP: ";
    html += WiFi.localIP().toString();
    html += "</p>";
  }

  html += "<form method=\"POST\" action=\"/save\">";
  html += "<label>WiFi SSID:</label><br>";
  html += "<select name=\"ssid\">";
  html += "<option value=\"\">(手动输入)</option>";
  html += buildNetworkOptionsHtml(savedSsid);
  html += "</select><br><br>";
  html += "<label>或手动输入 SSID:</label><br>";
  html += "<input name=\"ssid_manual\" value=\"";
  html += htmlEscape(savedSsid);
  html += "\"><br><br>";
  html += "<label>WiFi 密码:</label><br>";
  html += "<input name=\"pass\" type=\"password\" value=\"";
  html += htmlEscape(savedPass);
  html += "\"><br><br>";
  html += "<label>ThingSpeak 发送间隔 (时/分/秒):</label><br>";
  html += "<input name=\"ts_h\" type=\"number\" min=\"0\" value=\"";
  html += String(savedTsH);
  html += "\" style=\"width:5em\"> : ";
  html += "<input name=\"ts_m\" type=\"number\" min=\"0\" max=\"59\" value=\"";
  html += String(savedTsM);
  html += "\" style=\"width:5em\"> : ";
  html += "<input name=\"ts_s\" type=\"number\" min=\"0\" max=\"59\" value=\"";
  html += String(savedTsS);
  html += "\" style=\"width:5em\"><br>";
  html += "<p>提示：若三项都为 0，则使用默认间隔。</p>";
  html += "<button type=\"submit\">保存并连接</button>";
  html += "</form>";
  html += "<p><a href=\"/\">刷新</a></p>";
  html += "</body></html>";

  webServer.send(200, "text/html; charset=utf-8", html);
}

void handlePortalSave() {
  const String ssidFromList = webServer.arg("ssid");
  const String ssidManual = webServer.arg("ssid_manual");
  const String pass = webServer.arg("pass");
  const uint32_t tsH = parseU32OrZero(webServer.arg("ts_h"));
  const uint32_t tsM = parseU32OrZero(webServer.arg("ts_m"));
  const uint32_t tsS = parseU32OrZero(webServer.arg("ts_s"));

  String ssid = ssidFromList;
  if (ssid.length() == 0) {
    ssid = ssidManual;
  }
  ssid.trim();
  saveThingSpeakIntervalHms(tsH, tsM, tsS);
  const uint32_t newInterval = computeThingSpeakIntervalMsFromPrefs();
  portENTER_CRITICAL(&settingsMux);
  thingSpeakIntervalMs = newInterval;
  portEXIT_CRITICAL(&settingsMux);

  bool willTryConnect = false;
  if (ssid.length() != 0) {
    saveWiFiCredentials(ssid, pass);
    forcePortalMode = false;
    wifiReconnectRequested = true;
    willTryConnect = true;
  }

  String html;
  html.reserve(512);
  html += "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>EcoFishTank WiFi</title></head><body>";
  if (willTryConnect) {
    html += "<p>已保存，正在后台尝试连接: ";
    html += htmlEscape(ssid);
    html += "</p>";
  } else {
    html += "<p>已保存 ThingSpeak 间隔设置。</p>";
  }
  html += "<p>请保持连接此热点，稍后点 <a href=\"/\">刷新</a> 查看状态。</p>";
  html += "</body></html>";

  webServer.send(200, "text/html; charset=utf-8", html);
}

void startConfigPortal() {
  if (portalActive) return;
  if (portalMutex != nullptr) {
    xSemaphoreTakeRecursive(portalMutex, portMAX_DELAY);
  }

  portalApSsid = String("EcoFishTank-") + chipSuffix();
  WiFi.mode(WIFI_AP_STA);
  const bool ok = WiFi.softAP(portalApSsid.c_str(), CONFIG_AP_PASSWORD, CONFIG_AP_CHANNEL, CONFIG_AP_HIDDEN,
                              CONFIG_AP_MAX_CONN);

  const IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(53, "*", apIp);

  webServer.on("/", HTTP_GET, handlePortalRoot);
  webServer.on("/save", HTTP_POST, handlePortalSave);
  webServer.onNotFound(handlePortalRoot);
  webServer.begin();

  portalActive = true;

  WiFi.scanNetworks(true);

  Serial.print("ConfigAP OK=");
  Serial.print(ok ? "1" : "0");
  Serial.print(" SSID=");
  Serial.print(portalApSsid);
  Serial.print(" PASS=");
  Serial.print(CONFIG_AP_PASSWORD);
  Serial.print(" IP=");
  Serial.println(apIp);

  if (portalMutex != nullptr) {
    xSemaphoreGiveRecursive(portalMutex);
  }
}

float clampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float adcToVoltage(int adcValue) {
  const int clipped = clampInt(adcValue, 0, ADC_MAX_VALUE);
  return (static_cast<float>(clipped) / static_cast<float>(ADC_MAX_VALUE)) * ADC_REFERENCE_VOLTAGE;
}

float readTemperatureC() {
  ds18b20.requestTemperatures();
  const float c = ds18b20.getTempCByIndex(0);
  if (c <= -127.0f || c >= 125.0f) {
    return NAN;
  }
  return c;
}

float readTdsPpm(float temperatureC) {
  constexpr int samples = 30;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += static_cast<uint32_t>(analogRead(PIN_TDS_ADC));
    delay(5);
  }

  const float rawAdc = static_cast<float>(sum) / static_cast<float>(samples);
  const float voltage = adcToVoltage(static_cast<int>(rawAdc));

  float compensationCoefficient = 1.0f;
  if (isfinite(temperatureC)) {
    compensationCoefficient = 1.0f + 0.02f * (temperatureC - 25.0f);
  }
  const float compensationVoltage = voltage / compensationCoefficient;

  const float tds =
      (133.42f * compensationVoltage * compensationVoltage * compensationVoltage -
       255.86f * compensationVoltage * compensationVoltage +
       857.39f * compensationVoltage) *
      0.5f * TDS_K_VALUE;

  return clampFloat(tds, 0.0f, 5000.0f);
}

float readLightPercent() {
  constexpr int samples = 10;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += static_cast<uint32_t>(analogRead(PIN_LIGHT_ADC));
    delay(2);
  }
  const int raw = static_cast<int>(sum / samples);
  const int clipped = clampInt(raw, LIGHT_ADC_MIN, LIGHT_ADC_MAX);
  const float pct = (static_cast<float>(LIGHT_ADC_MAX - clipped) /
                     static_cast<float>(LIGHT_ADC_MAX - LIGHT_ADC_MIN)) *
                    100.0f;
  return clampFloat(pct, 0.0f, 100.0f);
}

float measureDistanceCm() {
  digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_ULTRASONIC_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_ULTRASONIC_TRIG, LOW);

  const unsigned long durationUs = pulseIn(PIN_ULTRASONIC_ECHO, HIGH, 30000);
  if (durationUs == 0) {
    return NAN;
  }
  return (static_cast<float>(durationUs) * 0.0343f) / 2.0f;
}

float computeWaterLevelCm(float distanceToWaterCm) {
  if (!isfinite(distanceToWaterCm)) {
    return NAN;
  }
  const float level = TANK_DEPTH_CM - distanceToWaterCm;
  return clampFloat(level, 0.0f, TANK_DEPTH_CM);
}

bool sendToThingSpeak(float temperatureC, float heightCm, float tdsPpm, float lightPercent) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String url = THINGSPEAK_USE_HTTPS ? "https://api.thingspeak.com/update?api_key=" : "http://api.thingspeak.com/update?api_key=";
  url += THINGSPEAK_WRITE_API_KEY;

  if (isfinite(temperatureC)) {
    url += "&field1=";
    url += String(temperatureC, 2);
  }
  if (isfinite(heightCm)) {
    url += "&field2=";
    url += String(heightCm, 1);
  }
  if (isfinite(tdsPpm)) {
    url += "&field3=";
    url += String(tdsPpm, 1);
  }
  if (isfinite(lightPercent)) {
    url += "&field4=";
    url += String(lightPercent, 1);
  }

  HTTPClient http;
  http.setTimeout(static_cast<int>(THINGSPEAK_HTTP_TIMEOUT_MS));
  http.begin(url);
  const int httpCode = http.GET();
  http.end();
  return httpCode == 200;
}

void printReadings() {
  Serial.print("WiFi=");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("OK ");
    Serial.print(WiFi.localIP());
  } else {
    Serial.print(portalActive ? "AP" : "OFF");
  }
  Serial.print(" TempC=");
  Serial.print(isfinite(lastTemperatureC) ? String(lastTemperatureC, 2) : String("nan"));
  Serial.print(" TDSppm=");
  Serial.print(isfinite(lastTdsPpm) ? String(lastTdsPpm, 1) : String("nan"));
  Serial.print(" WaterLevelCm=");
  Serial.print(isfinite(lastWaterLevelCm) ? String(lastWaterLevelCm, 1) : String("nan"));
  Serial.print(" Light%=");
  Serial.println(isfinite(lastLightPercent) ? String(lastLightPercent, 1) : String("nan"));
}

String formatNumberOrDash(float v, int decimals) {
  if (!isfinite(v)) return "-";
  return String(v, decimals);
}

void updateDisplay() {
  if (!displayOk) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.print("Temperature:");
  display.print(formatNumberOrDash(lastTemperatureC, 1));
  display.println("C");
  display.print("Lightness:");
  display.print(formatNumberOrDash(lastLightPercent, 0));
  display.println("%");

  display.print("TDS:");
  display.print(formatNumberOrDash(lastTdsPpm, 0));
  display.println("ppm");

  display.print("Water Level:");
  display.print(formatNumberOrDash(lastWaterLevelCm, 1));
  display.println("cm");

  display.print("WiFi:");
  if (WiFi.status() == WL_CONNECTED) display.println("OK");
  else if (portalActive) display.println("AP");
  else display.println("OFF");

  if (WiFi.status() == WL_CONNECTED) {
    display.print("IP:");
    display.println(WiFi.localIP());
  } else if (portalActive) {
    display.println("Cfg:192.168.4.1");
  } else {
    display.println("");
  }

  display.print("IoT State:");
  display.print(lastThingSpeakOk ? "OK" : "--");

  display.display();
}

void sensorTask(void*) {
  for (;;) {
    const float temperatureC = readTemperatureC();
    const float tdsPpm = readTdsPpm(temperatureC);
    const float lightPercent = readLightPercent();
    const float waterLevelCm = computeWaterLevelCm(measureDistanceCm());

    portENTER_CRITICAL(&readingsMux);
    lastTemperatureC = temperatureC;
    lastTdsPpm = tdsPpm;
    lastLightPercent = lightPercent;
    lastWaterLevelCm = waterLevelCm;
    portEXIT_CRITICAL(&readingsMux);

    printReadings();
    updateDisplay();

    vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_INTERVAL_MS));
  }
}

void portalTask(void*) {
  for (;;) {
    if (!portalActive) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (portalMutex != nullptr) {
      xSemaphoreTakeRecursive(portalMutex, portMAX_DELAY);
    }
    dnsServer.processNextRequest();
    webServer.handleClient();
    if (portalMutex != nullptr) {
      xSemaphoreGiveRecursive(portalMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void buttonTask(void*) {
  bool lastStablePressed = false;
  bool lastRawPressed = false;
  uint32_t lastChangeMs = millis();

  for (;;) {
    const int raw = digitalRead(PIN_USER_BUTTON);
    const bool pressed = USER_BUTTON_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);

    const uint32_t now = millis();
    if (pressed != lastRawPressed) {
      lastRawPressed = pressed;
      lastChangeMs = now;
    }

    if (now - lastChangeMs >= USER_BUTTON_DEBOUNCE_MS) {
      if (pressed != lastStablePressed) {
        lastStablePressed = pressed;
        if (pressed) {
          portalButtonRequested = true;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void networkTask(void*) {
  uint32_t lastUploadAttemptMs = 0;
  uint32_t lastConnectAttemptMs = 0;
  uint32_t connectStartMs = 0;
  bool connecting = false;
  String ssid;
  String pass;

  for (;;) {
    const uint32_t now = millis();

    if (portalButtonRequested) {
      portalButtonRequested = false;
      forcePortalMode = true;
      connecting = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_AP_STA);
      startConfigPortal();
      Serial.println("Portal=FORCED_BY_BUTTON");
    }

    if (wifiReconnectRequested) {
      wifiReconnectRequested = false;
      connecting = false;
      WiFi.disconnect(true);
      lastConnectAttemptMs = 0;
    }

    if (WiFi.status() == WL_CONNECTED) {
      connecting = false;
      if (portalActive && !forcePortalMode) {
        stopConfigPortal();
      }
    } else {
      loadWiFiCredentials(ssid, pass);

      if (!portalActive) {
        startConfigPortal();
      }

      if (forcePortalMode || ssid.length() == 0) {
        connecting = false;
      } else {
        if (!connecting && (now - lastConnectAttemptMs >= 5000)) {
          WiFi.mode(WIFI_AP_STA);
          WiFi.begin(ssid.c_str(), pass.c_str());
          connecting = true;
          connectStartMs = now;
          lastConnectAttemptMs = now;
        } else if (connecting && (now - connectStartMs >= WIFI_CONNECT_TIMEOUT_MS)) {
          WiFi.disconnect(false);
          connecting = false;
        }
      }
    }

    uint32_t uploadIntervalMs;
    portENTER_CRITICAL(&settingsMux);
    uploadIntervalMs = thingSpeakIntervalMs;
    portEXIT_CRITICAL(&settingsMux);

    if (WiFi.status() == WL_CONNECTED && (now - lastUploadAttemptMs >= uploadIntervalMs)) {
      float temperatureC;
      float heightCm;
      float tdsPpm;
      float lightPercent;

      portENTER_CRITICAL(&readingsMux);
      temperatureC = lastTemperatureC;
      heightCm = lastWaterLevelCm;
      tdsPpm = lastTdsPpm;
      lightPercent = lastLightPercent;
      portEXIT_CRITICAL(&readingsMux);

      lastThingSpeakOk = sendToThingSpeak(temperatureC, heightCm, tdsPpm, lightPercent);
      Serial.println(lastThingSpeakOk ? "ThingSpeak=OK" : "ThingSpeak=FAIL");
      updateDisplay();
      lastUploadAttemptMs = now;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

}

void setup() {
  Serial.begin(115200);
  WiFi.setSleep(false);

  pinMode(PIN_USER_BUTTON, INPUT_PULLUP);

  pinMode(PIN_ULTRASONIC_TRIG, OUTPUT);
  pinMode(PIN_ULTRASONIC_ECHO, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
  if (displayOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("EcoFishTank");
    display.display();
  }

  ds18b20.begin();

  preferences.begin("wifi", false);
  portENTER_CRITICAL(&settingsMux);
  thingSpeakIntervalMs = computeThingSpeakIntervalMsFromPrefs();
  portEXIT_CRITICAL(&settingsMux);

  portalMutex = xSemaphoreCreateRecursiveMutex();
  startConfigPortal();

  xTaskCreatePinnedToCore(sensorTask, "EcoSensor", 4096, nullptr, 1, &sensorTaskHandle, 1);
  xTaskCreatePinnedToCore(portalTask, "EcoPortal", 4096, nullptr, 1, &portalTaskHandle, 0);
  xTaskCreatePinnedToCore(networkTask, "EcoNetwork", 6144, nullptr, 1, &networkTaskHandle, 0);
  xTaskCreatePinnedToCore(buttonTask, "EcoButton", 2048, nullptr, 1, &buttonTaskHandle, 1);
}

void loop() {
  delay(1000);
}
