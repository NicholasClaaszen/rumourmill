#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_Thermal.h>
#include <vector>

/*
  V&V Rumour mill

  Self-contained AP + web UI for managing rumors and printing via thermal printer.

  Components:
   1x ESP32 (most versions will work)
   1x QR204 58mm thermal panel printer
   1x 2A 5v powersupply

  Connections:
    Printer     ESP32
    RX   -->    TX2
    TX   -->    RX2
    GND  -->    GND
    Connect 5v and GND to powersupply 5V and GND
*/

static const char *kApSsid = "RumourMill";
static const char *kApPassword = "OhNoSheDidnt";
static const char *kRumorsPath = "/rumors.json";

static const int kLedPin = 2;
static const int kReedPin = 4;
static const uint32_t kReedPollMs = 50;
static const uint32_t kPrintCooldownMs = 15000;

static const uint16_t kDefaultMaxPrints = 5;

Adafruit_Thermal printer(&Serial1);
AsyncWebServer server(80);
SemaphoreHandle_t rumorsMutex;
QueueHandle_t printQueue;

struct Rumor {
  uint32_t id = 0;
  String title;
  String textNl;
  String textEn;
  String people;
  bool active = true;
  uint16_t maxPrints = kDefaultMaxPrints;
  uint16_t printedCount = 0;
};

static std::vector<Rumor> rumors;

static void logLine(const char *message) {
  Serial.println(message);
}

static bool lockRumors(uint32_t timeoutMs) {
  return xSemaphoreTake(rumorsMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void unlockRumors() {
  xSemaphoreGive(rumorsMutex);
}

static uint32_t nextRumorId() {
  uint32_t maxId = 0;
  for (const auto &rumor : rumors) {
    if (rumor.id > maxId) {
      maxId = rumor.id;
    }
  }
  return maxId + 1;
}

static bool saveRumorsLocked() {
  DynamicJsonDocument doc(1024 + rumors.size() * 256);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &rumor : rumors) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = rumor.id;
    obj["title"] = rumor.title;
    obj["text_nl"] = rumor.textNl;
    obj["text_en"] = rumor.textEn;
    obj["people"] = rumor.people;
    obj["active"] = rumor.active;
    obj["max_prints"] = rumor.maxPrints;
    obj["printed_count"] = rumor.printedCount;
  }

  File file = LittleFS.open(kRumorsPath, "w");
  if (!file) {
    return false;
  }
  serializeJson(doc, file);
  file.close();
  return true;
}

static bool loadRumors() {
  if (!LittleFS.begin(true)) {
    logLine("[rumor] LittleFS begin failed");
    return false;
  }
  if (!LittleFS.exists(kRumorsPath)) {
    if (!lockRumors(200)) {
      logLine("[rumor] mutex busy on init");
      return false;
    }
    rumors.clear();
    bool ok = saveRumorsLocked();
    unlockRumors();
    if (ok) {
      logLine("[rumor] created empty rumors store");
    } else {
      logLine("[rumor] failed to create empty rumors store");
    }
    return ok;
  }

  File file = LittleFS.open(kRumorsPath, "r");
  if (!file) {
    logLine("[rumor] failed to open rumors file");
    return false;
  }

  size_t size = file.size();
  DynamicJsonDocument doc(size + 1024);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.printf("[rumor] JSON parse failed: %s\n", err.c_str());
    return false;
  }

  if (!lockRumors(200)) {
    logLine("[rumor] mutex busy while loading");
    return false;
  }
  rumors.clear();
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    Rumor rumor;
    rumor.id = obj["id"] | 0;
    rumor.title = obj["title"] | "";
    rumor.textNl = obj["text_nl"] | "";
    rumor.textEn = obj["text_en"] | "";
    rumor.people = obj["people"] | "";
    rumor.active = obj["active"] | true;
    rumor.maxPrints = obj["max_prints"] | kDefaultMaxPrints;
    rumor.printedCount = obj["printed_count"] | 0;
    rumors.push_back(rumor);
  }
  unlockRumors();
  Serial.printf("[rumor] loaded %u rumors\n", static_cast<unsigned>(rumors.size()));

  return true;
}

static String toLowerCopy(const String &input) {
  String out = input;
  out.toLowerCase();
  return out;
}

static bool nameMatches(const Rumor &rumor, const String &needle) {
  if (needle.length() == 0) {
    return true;
  }
  String needleLower = toLowerCopy(needle);
  String peopleLower = toLowerCopy(rumor.people);

  int start = 0;
  while (start < peopleLower.length()) {
    int comma = peopleLower.indexOf(',', start);
    if (comma == -1) {
      comma = peopleLower.length();
    }
    String chunk = peopleLower.substring(start, comma);
    chunk.trim();
    if (chunk.length() > 0 && chunk.indexOf(needleLower) != -1) {
      return true;
    }
    start = comma + 1;
  }
  return false;
}

static void sendJsonError(AsyncWebServerRequest *request, int code, const char *message) {
  DynamicJsonDocument doc(256);
  doc["error"] = message;
  String payload;
  serializeJson(doc, payload);
  request->send(code, "application/json", payload);
}

static bool parseRumorFromJson(const JsonVariantConst &src, Rumor &rumor, bool allowPartial) {
  if (!allowPartial) {
    if (!src.containsKey("title") || !src.containsKey("text_nl") || !src.containsKey("text_en") ||
        !src.containsKey("people") || !src.containsKey("active")) {
      return false;
    }
  }

  if (src.containsKey("title")) {
    rumor.title = (const char *)src["title"];
  }
  if (src.containsKey("text_nl")) {
    rumor.textNl = (const char *)src["text_nl"];
  }
  if (src.containsKey("text_en")) {
    rumor.textEn = (const char *)src["text_en"];
  }
  if (src.containsKey("people")) {
    rumor.people = (const char *)src["people"];
  }
  if (src.containsKey("active")) {
    rumor.active = src["active"].as<bool>();
  }
  if (src.containsKey("max_prints")) {
    uint16_t maxPrints = src["max_prints"] | kDefaultMaxPrints;
    if (maxPrints < 1) {
      maxPrints = 1;
    }
    rumor.maxPrints = maxPrints;
  }
  return true;
}

static void appendRumorJson(JsonArray arr, const Rumor &rumor) {
  JsonObject obj = arr.createNestedObject();
  obj["id"] = rumor.id;
  obj["title"] = rumor.title;
  obj["text_nl"] = rumor.textNl;
  obj["text_en"] = rumor.textEn;
  obj["people"] = rumor.people;
  obj["active"] = rumor.active;
  obj["max_prints"] = rumor.maxPrints;
  obj["printed_count"] = rumor.printedCount;
}

static void handleListRumors(AsyncWebServerRequest *request) {
  String nameFilter;
  if (request->hasParam("name")) {
    nameFilter = request->getParam("name")->value();
  }

  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }

  DynamicJsonDocument doc(1024 + rumors.size() * 256);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &rumor : rumors) {
    if (nameMatches(rumor, nameFilter)) {
      appendRumorJson(arr, rumor);
    }
  }
  unlockRumors();

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

static void handleCreateRumor(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    request->_tempObject = new String();
  }
  String *body = static_cast<String *>(request->_tempObject);
  body->concat(reinterpret_cast<char *>(data), len);
  if (index + len != total) {
    return;
  }

  DynamicJsonDocument doc(body->length() + 512);
  DeserializationError err = deserializeJson(doc, *body);
  delete body;
  request->_tempObject = nullptr;
  if (err) {
    sendJsonError(request, 400, "invalid json");
    return;
  }

  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }

  Rumor rumor;
  rumor.id = nextRumorId();
  rumor.maxPrints = kDefaultMaxPrints;
  if (!parseRumorFromJson(doc.as<JsonVariantConst>(), rumor, false)) {
    unlockRumors();
    sendJsonError(request, 400, "missing fields");
    return;
  }
  rumors.push_back(rumor);
  saveRumorsLocked();
  unlockRumors();

  DynamicJsonDocument out(512);
  JsonArray arr = out.to<JsonArray>();
  appendRumorJson(arr, rumor);
  String payload;
  serializeJson(arr[0], payload);
  request->send(201, "application/json", payload);
}

static void handleUpdateRumor(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    request->_tempObject = new String();
  }
  String *body = static_cast<String *>(request->_tempObject);
  body->concat(reinterpret_cast<char *>(data), len);
  if (index + len != total) {
    return;
  }

  uint32_t rumorId = request->pathArg(0).toInt();
  DynamicJsonDocument doc(body->length() + 512);
  DeserializationError err = deserializeJson(doc, *body);
  delete body;
  request->_tempObject = nullptr;
  if (err) {
    sendJsonError(request, 400, "invalid json");
    return;
  }

  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }

  Rumor *target = nullptr;
  for (auto &rumor : rumors) {
    if (rumor.id == rumorId) {
      target = &rumor;
      break;
    }
  }
  if (!target) {
    unlockRumors();
    sendJsonError(request, 404, "not found");
    return;
  }

  if (!parseRumorFromJson(doc.as<JsonVariantConst>(), *target, true)) {
    unlockRumors();
    sendJsonError(request, 400, "missing fields");
    return;
  }
  saveRumorsLocked();
  Rumor updated = *target;
  unlockRumors();

  String payload;
  DynamicJsonDocument out(512);
  JsonArray arr = out.to<JsonArray>();
  appendRumorJson(arr, updated);
  serializeJson(arr[0], payload);
  request->send(200, "application/json", payload);
}

static void handleDeleteRumor(AsyncWebServerRequest *request) {
  uint32_t rumorId = request->pathArg(0).toInt();
  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }

  bool removed = false;
  for (auto it = rumors.begin(); it != rumors.end(); ++it) {
    if (it->id == rumorId) {
      rumors.erase(it);
      removed = true;
      break;
    }
  }
  if (removed) {
    saveRumorsLocked();
  }
  unlockRumors();

  if (!removed) {
    sendJsonError(request, 404, "not found");
    return;
  }
  request->send(204);
}

static void handleResetRumor(AsyncWebServerRequest *request) {
  uint32_t rumorId = request->pathArg(0).toInt();
  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }

  Rumor *target = nullptr;
  for (auto &rumor : rumors) {
    if (rumor.id == rumorId) {
      target = &rumor;
      break;
    }
  }
  if (!target) {
    unlockRumors();
    sendJsonError(request, 404, "not found");
    return;
  }

  target->printedCount = 0;
  saveRumorsLocked();
  unlockRumors();
  request->send(204);
}

static void handleResetAllRumors(AsyncWebServerRequest *request) {
  if (!lockRumors(500)) {
    sendJsonError(request, 503, "busy");
    return;
  }
  for (auto &rumor : rumors) {
    rumor.printedCount = 0;
  }
  saveRumorsLocked();
  unlockRumors();
  request->send(204);
}

static void setupRoutes() {
  server.on("/api/rumors", HTTP_GET, handleListRumors);

  server.on("/api/rumors", HTTP_POST, [](AsyncWebServerRequest *request) {},
            nullptr, handleCreateRumor);

  server.on("^\\/api\\/rumors\\/(\\d+)$", HTTP_PUT, [](AsyncWebServerRequest *request) {},
            nullptr, handleUpdateRumor);

  server.on("^\\/api\\/rumors\\/(\\d+)$", HTTP_DELETE, handleDeleteRumor);
  server.on("^\\/api\\/rumors\\/(\\d+)\\/reset$", HTTP_POST, handleResetRumor);
  server.on("/api/rumors/resetAll", HTTP_POST, handleResetAllRumors);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_GET) {
      request->send(LittleFS, "/index.html", "text/html");
      return;
    }
    request->send(404);
  });
}

static void printStart() {
  printer.boldOn();
  printer.feed(2);
  delay(10);
  printer.println("Rumour Mill");
  delay(10);
  printer.println("Connect to:");
  delay(10);
  printer.println(kApSsid);
  delay(10);
  printer.println("Open:");
  delay(10);
  printer.println(WiFi.softAPIP());
  delay(10);
  printer.feed(4);
  delay(10);
  printer.sleep();
  delay(1000);
  printer.wake();
}

static void printRumor(const Rumor &rumor) {
  printer.boldOn();
  printer.feed(2);
  delay(10);
  printer.println(rumor.textNl);
  delay(10);
  printer.println(rumor.textEn);
  delay(10);
  printer.feed(10);
  delay(10);
  printer.sleep();
  delay(1000);
  printer.wake();
}

static bool pickRandomRumor(Rumor &selected) {
  if (!lockRumors(500)) {
    return false;
  }
  std::vector<size_t> eligible;
  for (size_t i = 0; i < rumors.size(); ++i) {
    const auto &rumor = rumors[i];
    if (!rumor.active) {
      continue;
    }
    if (rumor.printedCount >= rumor.maxPrints) {
      continue;
    }
    eligible.push_back(i);
  }
  if (eligible.empty()) {
    unlockRumors();
    return false;
  }

  size_t choice = eligible[random(eligible.size())];
  rumors[choice].printedCount += 1;
  selected = rumors[choice];
  saveRumorsLocked();
  unlockRumors();
  return true;
}

static void printTask(void *parameter) {
  uint8_t signal = 0;
  for (;;) {
    if (xQueueReceive(printQueue, &signal, portMAX_DELAY) == pdTRUE) {
      Serial.println("[print] trigger received");
      Rumor selected;
      if (pickRandomRumor(selected)) {
        Serial.printf("[print] printing rumor id=%u title=%s\n", selected.id, selected.title.c_str());
        printRumor(selected);
      } else {
        logLine("[print] no eligible rumors");
        printer.boldOn();
        printer.feed(2);
        delay(10);
        printer.println("No active rumors");
        delay(10);
        printer.println("or max prints reached");
        delay(10);
        printer.feed(6);
        delay(10);
        printer.sleep();
        delay(1000);
        printer.wake();
      }
    }
  }
}

static void reedTask(void *parameter) {
  int lastState = digitalRead(kReedPin);
  uint32_t lastTrigger = 0;
  for (;;) {
    int state = digitalRead(kReedPin);
    uint32_t now = millis();
    if (state == LOW && lastState == HIGH && (now - lastTrigger) > kPrintCooldownMs) {
      uint8_t signal = 1;
      xQueueSend(printQueue, &signal, 0);
      lastTrigger = now;
      Serial.println("[reed] trigger queued");
    }
    lastState = state;
    vTaskDelay(pdMS_TO_TICKS(kReedPollMs));
  }
}

void setup() {
  pinMode(kLedPin, OUTPUT);
  pinMode(kReedPin, INPUT_PULLUP);
  Serial.begin(115200);
  logLine("[setup] booting");

  Serial1.begin(9600, SERIAL_8N1, 16, 17);
  printer.setTimes(200, 200);
  logLine("[setup] serial1/printer ready");

  rumorsMutex = xSemaphoreCreateMutex();
  printQueue = xQueueCreate(4, sizeof(uint8_t));
  logLine("[setup] RTOS primitives ready");

  if (!loadRumors()) {
    Serial.println("Failed to load rumors.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  Serial.printf("[wifi] AP up: %s\n", kApSsid);
  Serial.printf("[wifi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  setupRoutes();
  server.begin();
  logLine("[web] server started");

  digitalWrite(kLedPin, HIGH);
  logLine("[setup] LED on, printing startup slip");
  printStart();

  xTaskCreatePinnedToCore(reedTask, "reedTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(printTask, "printTask", 6144, nullptr, 1, nullptr, 1);
  logLine("[setup] tasks started");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
