#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include "secrets.h"

const char* OPENAI_URL       = "https://api.openai.com/v1/chat/completions";
const char* MQTT_BROKER      = "broker.hivemq.com";
const int   MQTT_PORT        = 1883;
const char* MQTT_TOPIC_DATA  = "gas/leak/data";
const char* MQTT_TOPIC_ALERT = "gas/leak/alert";
const char* MQTT_TOPIC_STATS = "gas/leak/stats";

const char* OTA_HOSTNAME     = "gas-detector";
const char* AP_SSID          = "GasDetector-Setup";

#define GAS_SENSOR_PIN     34
#define SMOKE_SENSOR_PIN   35
#define DHT_PIN            4
#define DHT_TYPE           DHT22
#define SOLENOID_PIN       26
#define RELAY_PIN          27
#define BUZZER_PIN         14
#define LED_GREEN_PIN      12
#define LED_YELLOW_PIN     13
#define LED_RED_PIN        15
#define RESET_BUTTON_PIN   0
#define STATUS_LED_PIN     2

#define GAS_NORMAL_MAX       800
#define GAS_WARNING_MIN      800
#define GAS_CRITICAL_MIN     2000
#define SMOKE_CRITICAL_MIN   1800
#define TEMP_CRITICAL        60.0
#define HUMIDITY_WARNING     85.0

#define GRADIENT_WINDOW_SIZE    6
#define LEAK_GRADIENT_THRESHOLD 25
#define EMA_ALPHA               0.3

const unsigned long SENSOR_INTERVAL   = 500;
const unsigned long MQTT_INTERVAL     = 5000;
const unsigned long OPENAI_COOLDOWN   = 60000;
const unsigned long TELEGRAM_COOLDOWN = 30000;
const unsigned long STATS_INTERVAL    = 30000;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
HTTPClient http;
WebServer server(80);
Preferences prefs;
DHT dht(DHT_PIN, DHT_TYPE);

enum SystemState { STATE_NORMAL, STATE_WARNING, STATE_CRITICAL, STATE_RESET };
SystemState currentState = STATE_NORMAL;

int   gasReadings[GRADIENT_WINDOW_SIZE] = {0};
int   readingIndex = 0;
float emaGas = 0;
float previousEma = 0;
bool  mlLeakDetected = false;

float temperature = 0;
float humidity = 0;

unsigned long lastSensorRead   = 0;
unsigned long lastMqttPublish  = 0;
unsigned long lastOpenAICall   = 0;
unsigned long lastTelegramCall = 0;
unsigned long lastStatsCalc    = 0;

int  statGasMin = 4095;
int  statGasMax = 0;
long statGasSum = 0;
int  statGasCount = 0;
int  statAlertCount = 0;

String lastOpenAIMessage = "No message yet";

#define LOG_BUFFER_SIZE 20
String logBuffer[LOG_BUFFER_SIZE];
int logIndex = 0;

void addLog(String msg) {
  logBuffer[logIndex] = String(millis()) + ": " + msg;
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
}

void connectWiFi();
void reconnectMQTT();
void publishMqttData();
void publishStats();
void callOpenAI(int gas, int smoke);
void sendTelegram(String message);
void readSensorsAndUpdateState();
void updateMLBuffer(int value);
void detectEarlyLeak();
SystemState determineState(int gas, int smoke);
void transitionToState(SystemState newState, int gas, int smoke);
void updateOutputs();
void resetSystem();
void setupWebServer();
void setupOTA();
void handleRoot();
void handleStatus();
void handleLogs();
void handleReset();
void handleNotFound();
void updateStatistics(int gas);
String getStateString();

void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  pinMode(SOLENOID_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);

  digitalWrite(SOLENOID_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, HIGH);
  digitalWrite(LED_YELLOW_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);

  dht.begin();
  prefs.begin("gasdetector", false);

  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);

  setupOTA();
  setupWebServer();

  for (int i = 0; i < GRADIENT_WINDOW_SIZE; i++) gasReadings[i] = 0;

  addLog("System initialized");
  Serial.println("System initialized");
  Serial.print("Web interface: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  ArduinoOTA.handle();
  server.handleClient();

  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensorsAndUpdateState();
  }

  if (now - lastMqttPublish >= MQTT_INTERVAL) {
    lastMqttPublish = now;
    publishMqttData();
  }

  if (now - lastStatsCalc >= STATS_INTERVAL) {
    lastStatsCalc = now;
    publishStats();
  }

  if (digitalRead(RESET_BUTTON_PIN) == LOW && currentState == STATE_CRITICAL) {
    delay(50);
    if (digitalRead(RESET_BUTTON_PIN) == LOW) resetSystem();
  }

  digitalWrite(STATUS_LED_PIN, (now / 1000) % 2);
}

void readSensorsAndUpdateState() {
  int gasValue   = analogRead(GAS_SENSOR_PIN);
  int smokeValue = analogRead(SMOKE_SENSOR_PIN);

  float newTemp = dht.readTemperature();
  float newHum  = dht.readHumidity();
  if (!isnan(newTemp)) temperature = newTemp;
  if (!isnan(newHum))  humidity = newHum;

  updateMLBuffer(gasValue);
  detectEarlyLeak();
  updateStatistics(gasValue);

  SystemState newState = determineState(gasValue, smokeValue);
  if (newState != currentState) transitionToState(newState, gasValue, smokeValue);

  updateOutputs();

  static unsigned long lastLog = 0;
  if (millis() - lastLog > 2000) {
    lastLog = millis();
    Serial.printf("Gas:%d Smoke:%d T:%.1f H:%.1f EMA:%.1f ML:%s State:%d\n",
                  gasValue, smokeValue, temperature, humidity, emaGas,
                  mlLeakDetected ? "LEAK" : "OK", currentState);
  }
}

void updateMLBuffer(int value) {
  gasReadings[readingIndex] = value;
  readingIndex = (readingIndex + 1) % GRADIENT_WINDOW_SIZE;

  if (emaGas == 0) {
    emaGas = value;
  } else {
    previousEma = emaGas;
    emaGas = (EMA_ALPHA * value) + ((1 - EMA_ALPHA) * previousEma);
  }
}

void detectEarlyLeak() {
  float gradient = emaGas - previousEma;
  if (gradient > LEAK_GRADIENT_THRESHOLD && emaGas > 400) {
    mlLeakDetected = true;
    addLog("ML Early Detection: gradient=" + String(gradient, 1));
  } else {
    mlLeakDetected = false;
  }
}

void updateStatistics(int gas) {
  if (gas < statGasMin) statGasMin = gas;
  if (gas > statGasMax) statGasMax = gas;
  statGasSum += gas;
  statGasCount++;
}

SystemState determineState(int gas, int smoke) {
  if (gas >= GAS_CRITICAL_MIN || smoke >= SMOKE_CRITICAL_MIN ||
      mlLeakDetected || temperature >= TEMP_CRITICAL)
    return STATE_CRITICAL;

  if (gas >= GAS_WARNING_MIN || humidity >= HUMIDITY_WARNING)
    return STATE_WARNING;

  return STATE_NORMAL;
}

void transitionToState(SystemState newState, int gas, int smoke) {
  currentState = newState;

  switch (newState) {
    case STATE_WARNING:
      addLog("WARNING: elevated gas levels");
      Serial.println("WARNING: Elevated gas levels detected");
      break;

    case STATE_CRITICAL:
      addLog("CRITICAL: gas leak detected");
      Serial.println("CRITICAL: Gas leak detected");
      digitalWrite(SOLENOID_PIN, HIGH);
      digitalWrite(RELAY_PIN, HIGH);
      statAlertCount++;

      if (millis() - lastOpenAICall > OPENAI_COOLDOWN) {
        lastOpenAICall = millis();
        callOpenAI(gas, smoke);
      }
      if (millis() - lastTelegramCall > TELEGRAM_COOLDOWN) {
        lastTelegramCall = millis();
        sendTelegram("CRITICAL ALERT: Gas=" + String(gas) + " Smoke=" + String(smoke));
      }
      break;

    case STATE_NORMAL:
      addLog("NORMAL: system safe");
      Serial.println("NORMAL: System safe");
      digitalWrite(SOLENOID_PIN, LOW);
      digitalWrite(RELAY_PIN, LOW);
      break;

    case STATE_RESET:
      resetSystem();
      break;
  }
}

void updateOutputs() {
  digitalWrite(LED_GREEN_PIN, currentState == STATE_NORMAL ? HIGH : LOW);
  digitalWrite(LED_YELLOW_PIN, currentState == STATE_WARNING ? HIGH : LOW);

  if (currentState == STATE_CRITICAL) {
    static unsigned long lastBlink = 0;
    static bool blinkState = false;
    if (millis() - lastBlink > 200) {
      lastBlink = millis();
      blinkState = !blinkState;
      digitalWrite(LED_RED_PIN, blinkState);
      digitalWrite(BUZZER_PIN, blinkState);
    }
  } else {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void callOpenAI(int gasValue, int smokeValue) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("OpenAI: WiFi not connected");
    return;
  }
  if (String(OPENAI_API_KEY).length() == 0) {
    lastOpenAIMessage = "API key not set";
    addLog("OpenAI: key not configured");
    return;
  }

  http.begin(OPENAI_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);

  String prompt = "Generate a short urgent safety warning (max 15 words) for a gas leak. ";
  prompt += "Gas:" + String(gasValue) + " Smoke:" + String(smokeValue);
  prompt += " Temp:" + String(temperature, 1) + " Hum:" + String(humidity, 1);
  prompt += ". English only.";

  StaticJsonDocument<512> doc;
  doc["model"] = "gpt-3.5-turbo";
  doc["max_tokens"] = 50;
  JsonArray messages = doc.createNestedArray("messages");
  JsonObject msg = messages.createNestedObject();
  msg["role"] = "user";
  msg["content"] = prompt;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    String resp = http.getString();
    DynamicJsonDocument rdoc(2048);
    if (deserializeJson(rdoc, resp) == DeserializationError::Ok) {
      lastOpenAIMessage = rdoc["choices"][0]["message"]["content"].as<String>();
      addLog("OpenAI: " + lastOpenAIMessage);
      Serial.println("OpenAI: " + lastOpenAIMessage);
    }
  } else {
    addLog("OpenAI failed: " + http.errorToString(code));
  }
  http.end();
}

void sendTelegram(String message) {
  if (String(TELEGRAM_TOKEN).length() == 0 || String(TELEGRAM_CHAT).length() == 0) {
    addLog("Telegram: not configured");
    return;
  }

  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT) +
               "&text=" + message;

  http.begin(url);
  int code = http.GET();
  addLog("Telegram: code=" + String(code));
  http.end();
}

void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.mode(WIFI_STA);

  String storedSSID = prefs.getString("ssid", "");
  String storedPass = prefs.getString("pass", "");

  if (storedSSID.length() > 0) {
    WiFi.begin(storedSSID.c_str(), storedPass.c_str());
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed. Starting AP mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("AP IP: " + WiFi.softAPIP().toString());
  }
}

void reconnectMQTT() {
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();

  String clientId = "esp32-gas-" + String(random(0xFFFF), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.publish(MQTT_TOPIC_ALERT, "System online");
  }
}

void publishMqttData() {
  if (!mqttClient.connected()) return;

  int gasValue   = analogRead(GAS_SENSOR_PIN);
  int smokeValue = analogRead(SMOKE_SENSOR_PIN);

  StaticJsonDocument<300> doc;
  doc["gas"]     = gasValue;
  doc["smoke"]   = smokeValue;
  doc["temp"]    = temperature;
  doc["hum"]     = humidity;
  doc["ema_gas"] = round(emaGas);
  doc["ml_leak"] = mlLeakDetected;
  doc["state"]   = currentState;
  doc["uptime"]  = millis() / 1000;

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(MQTT_TOPIC_DATA, payload.c_str());

  if (currentState == STATE_CRITICAL) {
    mqttClient.publish(MQTT_TOPIC_ALERT, lastOpenAIMessage.c_str());
  }
}

void publishStats() {
  if (!mqttClient.connected() || statGasCount == 0) return;

  StaticJsonDocument<300> doc;
  doc["min"]     = statGasMin;
  doc["max"]     = statGasMax;
  doc["avg"]     = statGasSum / statGasCount;
  doc["alerts"]  = statAlertCount;
  doc["samples"] = statGasCount;

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(MQTT_TOPIC_STATS, payload.c_str());

  statGasMin = 4095;
  statGasMax = 0;
  statGasSum = 0;
  statGasCount = 0;
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogs);
  server.on("/reset", handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
  addLog("Web server started");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Gas Detector</title>";
  html += "<style>body{font-family:Arial;background:#1a1a1a;color:#eee;padding:20px;text-align:center}";
  html += ".card{background:#2a2a2a;padding:20px;border-radius:10px;margin:10px;display:inline-block;min-width:150px}";
  html += ".val{font-size:32px;font-weight:bold;color:#4CAF50}";
  html += "</style></head><body>";
  html += "<h1>Gas Leak Detection System</h1>";
  html += "<div class='card'><h3>State</h3><div class='val'>" + getStateString() + "</div></div>";
  html += "<div class='card'><h3>Gas</h3><div class='val'>" + String(analogRead(GAS_SENSOR_PIN)) + "</div></div>";
  html += "<div class='card'><h3>Smoke</h3><div class='val'>" + String(analogRead(SMOKE_SENSOR_PIN)) + "</div></div>";
  html += "<div class='card'><h3>Temp</h3><div class='val'>" + String(temperature, 1) + "C</div></div>";
  html += "<div class='card'><h3>Humidity</h3><div class='val'>" + String(humidity, 1) + "%</div></div>";
  html += "<div class='card'><h3>Alerts</h3><div class='val'>" + String(statAlertCount) + "</div></div>";
  html += "<div class='card'><h3>Uptime</h3><div class='val'>" + String(millis() / 1000) + "s</div></div>";
  html += "<p>Last OpenAI: " + lastOpenAIMessage + "</p>";
  html += "<p><a href='/logs' style='color:#4CAF50'>Logs</a> | ";
  html += "<a href='/status' style='color:#4CAF50'>JSON</a> | ";
  html += "<a href='/reset' style='color:#f44336'>Reset</a></p>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  StaticJsonDocument<400> doc;
  doc["state"]    = getStateString();
  doc["gas"]      = analogRead(GAS_SENSOR_PIN);
  doc["smoke"]    = analogRead(SMOKE_SENSOR_PIN);
  doc["temp"]     = temperature;
  doc["hum"]      = humidity;
  doc["ema_gas"]  = emaGas;
  doc["ml_leak"]  = mlLeakDetected;
  doc["alerts"]   = statAlertCount;
  doc["uptime_s"] = millis() / 1000;
  doc["last_msg"] = lastOpenAIMessage;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLogs() {
  String out = "[";
  for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
    int idx = (logIndex + i) % LOG_BUFFER_SIZE;
    if (logBuffer[idx].length() == 0) continue;
    if (out.length() > 1) out += ",";
    out += "\"" + logBuffer[idx] + "\"";
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleReset() {
  resetSystem();
  server.send(200, "text/plain", "Reset done");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

String getStateString() {
  switch (currentState) {
    case STATE_NORMAL:   return "NORMAL";
    case STATE_WARNING:  return "WARNING";
    case STATE_CRITICAL: return "CRITICAL";
    case STATE_RESET:    return "RESET";
  }
  return "UNKNOWN";
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (String(OTA_PASSWORD).length() > 0) ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { addLog("OTA start"); });
  ArduinoOTA.onEnd([]()   { addLog("OTA end"); });
  ArduinoOTA.onError([](ota_error_t e) { addLog("OTA error: " + String(e)); });

  ArduinoOTA.begin();
  addLog("OTA ready");
}

void resetSystem() {
  currentState = STATE_NORMAL;
  mlLeakDetected = false;
  emaGas = 0;
  previousEma = 0;

  digitalWrite(SOLENOID_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, HIGH);
  digitalWrite(LED_YELLOW_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);

  for (int i = 0; i < GRADIENT_WINDOW_SIZE; i++) gasReadings[i] = 0;
  readingIndex = 0;

  addLog("System reset");
  Serial.println("System reset to NORMAL");
}