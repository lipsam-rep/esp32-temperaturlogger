#include <WiFi.h>
#include <WiFiManager.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <vector>

// -------------------------------------------------
// Hardware-Konstanten
// -------------------------------------------------
#define ONE_WIRE_PIN 4

#define LED_PIN 2
#define LED_ACTIVE_LOW true

// Zeitzone (für NTP/TLS-Zertifikate relevant)
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/03:00:00"

// -------------------------------------------------
// Standardwerte
// -------------------------------------------------
static const char* INFLUXDB_URL_DEFAULT     = "https://eu-central-1-1.aws.cloud2.influxdata.com";
static const char* INFLUXDB_BUCKET_DEFAULT  = "BucketMeaserments";
static const char* DEFAULT_MEASUREMENT      = "temperature";

static const int   DEFAULT_TEMP_RES            = 10;          // 9..12
static const unsigned long DEFAULT_INTERVAL_MS = 30000;
static const unsigned long DEFAULT_IDENT_INTERVAL_MS = 30000;

static const int   MAX_ROOMS = 32;




// -------------------------------------------------
// Mode
// -------------------------------------------------
enum class Mode : uint8_t {
  Operate  = 0,
  Identify = 1
};

static const char* modeToString(Mode currentMode) {
  switch (currentMode) {
    case Mode::Operate:  return "operate";
    case Mode::Identify: return "identify";
  }
  return "unknown";
}

static bool parseModeFromInput(const String& inputMode, Mode& setMode) {
  String normalized = inputMode;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "operate")  { setMode = Mode::Operate;  return true; }
  if (normalized == "identify") { setMode = Mode::Identify; return true; }

  return false;
}

// -------------------------------------------------
// Persistente Konfiguration (LittleFS /config.json)
// -------------------------------------------------
struct Config {
  String url;
  String token;
  String org;
  String bucket;
  String measurement;

  String deviceId;   // read-only, aus STA-MAC abgeleitet
  String alias;      // editierbar

  int tempResBits;
  unsigned long operateIntervalMs;
  unsigned long identifyIntervalMs;

  Mode currentMode;

  std::vector<String> rooms;
};

Config cfg;
bool cfgLoaded = false;

// -------------------------------------------------
// DS18B20 / OneWire
// -------------------------------------------------
OneWire oneWireBus(ONE_WIRE_PIN);
DallasTemperature temperatureBus(&oneWireBus);
DeviceAddress sensorAddress;

// -------------------------------------------------
// InfluxDB (optional)
// -------------------------------------------------
InfluxDBClient* influxClient = nullptr;

// -------------------------------------------------
// Webserver
// -------------------------------------------------
WebServer httpServer(80);

// -------------------------------------------------
// Timing
// -------------------------------------------------
unsigned long lastSampleMs = 0;

// -------------------------------------------------
// Forward Declarations (damit Reihenfolge egal ist)
// -------------------------------------------------
void setStatusLed(bool on);
void pulseStatusLed(int times, int onMs, int offMs);
void signalCycleOk();
void signalCycleFail();

String romToString(const DeviceAddress romAddress);
String sensorIdFromIndex(int indexZeroBased);
String roomForSensorIndex(int sensorIndex);

String getStaMacHexUpper();
String deriveDeviceIdFromStaMac();
bool enforceDeviceIdFromStaMac();

// Kompatibilitäts-Wrapper: im Code wurden teils alte Namen benutzt
static inline bool forceDeviceIdFromMac() { return enforceDeviceIdFromStaMac(); }

void normalizeConfig();
bool influxConfigIsUsable();
void initInfluxClient();
void applyConfigToRuntime();

bool loadConfig();
bool saveConfig();

String toHtmlText(const String& in);
String currentModeLabel();

// Web Handlers
void handleRoot();
void handleConfigForm();
void handleSave();
void handleTest();
void handleReboot();
void handleApiTemps();
void handleRoomsForm();
void handleSaveRooms();
void registerRoutes();

bool shouldStartPortal();

// =================================================
// LED
// =================================================

// Schaltet die Status-LED ein oder aus (active LOW/HIGH abstrahiert).
void setStatusLed(bool on) {
  if (LED_PIN < 0) return;

  if (LED_ACTIVE_LOW) {
    digitalWrite(LED_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
  }
}

// Blinkmuster (blockierend) für Statussignale
void pulseStatusLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    setStatusLed(true);
    delay(onMs);
    setStatusLed(false);
    delay(offMs);
  }
}

void signalCycleOk()   { pulseStatusLed(2,  80,  80); }
void signalCycleFail() { pulseStatusLed(3, 250, 150); }



// =================================================
// Sensor-/Mapping Helper
// =================================================

// ROM (8 Bytes) → 16 Hex-Zeichen (Uppercase)
String romToString(const DeviceAddress romAddress) {
  String romHex;
  romHex.reserve(16);

  for (uint8_t byteIndex = 0; byteIndex < 8; byteIndex++) {
    if (romAddress[byteIndex] < 16) romHex += "0";
    romHex += String(romAddress[byteIndex], HEX);
  }

  romHex.toUpperCase();
  return romHex;
}

// Logischer Sensorname aus 0-basiertem Index: 0→S1, 1→S2, ...
String sensorIdFromIndex(int indexZeroBased) {
  return "S" + String(indexZeroBased + 1);
}

// Raumnamen aus cfg.rooms; falls leer/nicht gesetzt → Fallback "Sx"
String roomForSensorIndex(int sensorIndex) {
  if (sensorIndex >= 0 && sensorIndex < (int)cfg.rooms.size()) {
    String configuredRoomName = cfg.rooms[sensorIndex];
    configuredRoomName.trim();
    if (configuredRoomName.length() > 0) return configuredRoomName;
  }
  return sensorIdFromIndex(sensorIndex);
}

// =================================================
// DeviceId aus STA-MAC
// =================================================

// STA-MAC als 12 Hex-Zeichen ohne ":" und in Großbuchstaben
String getStaMacHexUpper() {
  String deviceMacHex = WiFi.macAddress(); // "88:57:21:CD:4E:E4"
  deviceMacHex.replace(":", "");           // "885721CD4EE4"
  deviceMacHex.toUpperCase();
  return deviceMacHex;
}

// DeviceId = "esp32-" + letzte 6 Hex-Zeichen (Suffix) der MAC
String deriveDeviceIdFromStaMac() {
  String macSuffix = getStaMacHexUpper();
  if (macSuffix.length() != 12) return "esp32-000000";
  return "esp32-" + macSuffix.substring(6);
}

// Erzwingt cfg.deviceId aus STA-MAC; true wenn geändert
bool enforceDeviceIdFromStaMac() {
  String derivedDeviceId = deriveDeviceIdFromStaMac();
  derivedDeviceId.trim();

  if (cfg.deviceId != derivedDeviceId) {
    cfg.deviceId = derivedDeviceId;
    return true;
  }
  return false;
}

// =================================================
// Config Normalisierung / Validierung
// =================================================

void normalizeConfig() {
  cfg.url.trim();
  cfg.bucket.trim();
  cfg.measurement.trim();
  cfg.org.trim();
  cfg.token.trim();
  cfg.deviceId.trim();
  cfg.alias.trim();

  if (cfg.url.length() == 0)         cfg.url = INFLUXDB_URL_DEFAULT;
  if (cfg.bucket.length() == 0)      cfg.bucket = INFLUXDB_BUCKET_DEFAULT;
  if (cfg.measurement.length() == 0) cfg.measurement = DEFAULT_MEASUREMENT;

  if (cfg.tempResBits < 9)  cfg.tempResBits = 9;
  if (cfg.tempResBits > 12) cfg.tempResBits = 12;

  if (cfg.operateIntervalMs < 5000)       cfg.operateIntervalMs = 5000;
  if (cfg.operateIntervalMs > 3600000UL)  cfg.operateIntervalMs = 3600000UL;

  if (cfg.identifyIntervalMs < 2000)      cfg.identifyIntervalMs = 2000;
  if (cfg.identifyIntervalMs > 3600000UL) cfg.identifyIntervalMs = 3600000UL;

  if ((int)cfg.rooms.size() > MAX_ROOMS) cfg.rooms.resize(MAX_ROOMS);
}

bool influxConfigIsUsable() {
  return cfg.url.length() &&
         cfg.bucket.length() &&
         cfg.org.length() &&
         cfg.token.length();
}

// =================================================
// Influx Client
// =================================================

void initInfluxClient() {
  if (influxClient) {
    delete influxClient;
    influxClient = nullptr;
  }

  influxClient = new InfluxDBClient(
    cfg.url, cfg.org, cfg.bucket, cfg.token, InfluxDbCloud2CACert
  );

  influxClient->setWriteOptions(WriteOptions().batchSize(50).flushInterval(1000));
}

// =================================================
// Runtime anwenden (Hardware/Services)
// =================================================

void applyConfigToRuntime() {
  // Sensorbus initialisieren und Auflösung setzen
  temperatureBus.begin();

  int sensorCount = temperatureBus.getDeviceCount();
  if (sensorCount < 0) sensorCount = 0;
  if (sensorCount > MAX_ROOMS) sensorCount = MAX_ROOMS;

  for (int i = 0; i < sensorCount; i++) {
    if (temperatureBus.getAddress(sensorAddress, i)) {
      temperatureBus.setResolution(sensorAddress, cfg.tempResBits);
    }
  }

  // Influx je nach Mode/Creds aktivieren/deaktivieren
  if (cfg.currentMode == Mode::Identify) {
    if (influxClient) { delete influxClient; influxClient = nullptr; }
  } else {
    if (influxConfigIsUsable()) {
      initInfluxClient();
      // Verbindung prüfen ist optional, aber hilfreich
      if (!influxClient->validateConnection()) {
        Serial.print("Influx validation failed: ");
        Serial.println(influxClient->getLastErrorMessage());
        signalCycleFail();
      } else {
        Serial.print("Influx connected: ");
        Serial.println(influxClient->getServerUrl());
      }
    } else {
      if (influxClient) { delete influxClient; influxClient = nullptr; }
    }
  }

  // Intervall-Timer neu starten
  lastSampleMs = millis();
}

// =================================================
// Config Load/Save
// =================================================

bool loadConfig() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists("/config.json")) return false;

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return false;

  JsonDocument configJson;
  DeserializationError parseError = deserializeJson(configJson, configFile);
  configFile.close();
  if (parseError) return false;

  cfg.url         = configJson["url"]         | INFLUXDB_URL_DEFAULT;
  cfg.token       = configJson["token"]       | "";
  cfg.org         = configJson["org"]         | "";
  cfg.bucket      = configJson["bucket"]      | INFLUXDB_BUCKET_DEFAULT;
  cfg.measurement = configJson["measurement"] | DEFAULT_MEASUREMENT;

  cfg.deviceId    = configJson["deviceId"]    | "";
  cfg.alias       = configJson["alias"]       | "";

  cfg.tempResBits        = configJson["tempResBits"]        | DEFAULT_TEMP_RES;
  cfg.operateIntervalMs  = configJson["operateIntervalMs"]  | DEFAULT_INTERVAL_MS;
  cfg.identifyIntervalMs = configJson["identifyIntervalMs"] | DEFAULT_IDENT_INTERVAL_MS;

  {
    String inputMode = configJson["mode"] | "operate";
    Mode setMode;
    cfg.currentMode = parseModeFromInput(inputMode, setMode) ? setMode : Mode::Operate;
  }

  cfg.rooms.clear();
  if (configJson["rooms"].is<JsonArray>()) {
    JsonArray a = configJson["rooms"].as<JsonArray>();
    for (JsonVariant v : a) {
      if ((int)cfg.rooms.size() >= MAX_ROOMS) break;
      cfg.rooms.push_back(String((const char*)(v | "")));
    }
  }

  normalizeConfig();
  return true;
}

bool saveConfig() {
  JsonDocument WriteJsonConfig;
  WriteJsonConfig["url"]         = cfg.url;
  WriteJsonConfig["token"]       = cfg.token;
  WriteJsonConfig["org"]         = cfg.org;
  WriteJsonConfig["bucket"]      = cfg.bucket;
  WriteJsonConfig["measurement"] = cfg.measurement;

  WriteJsonConfig["deviceId"]    = cfg.deviceId;
  WriteJsonConfig["alias"]       = cfg.alias;

  WriteJsonConfig["tempResBits"]        = cfg.tempResBits;
  WriteJsonConfig["operateIntervalMs"]  = cfg.operateIntervalMs;
  WriteJsonConfig["identifyIntervalMs"] = cfg.identifyIntervalMs;
  WriteJsonConfig["mode"]               = modeToString(cfg.currentMode);

  JsonArray rooms = WriteJsonConfig ["rooms"].to<JsonArray>();
  for (size_t i = 0; i < cfg.rooms.size(); i++) rooms.add(cfg.rooms[i]);

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) return false;
  serializeJson(WriteJsonConfig, configFile);
  configFile.close();
  return true;
}

// =================================================
// Web Helper
// =================================================

String toHtmlText(const String& rawText) {

  // Ergebnistext für HTML-Ausgabe
  String htmlText;
  htmlText.reserve(rawText.length());

  for (size_t charIndex = 0; charIndex < rawText.length(); charIndex++) {
    const char currentChar = rawText[charIndex];

    if (currentChar == '&')       htmlText += "&amp;";
    else if (currentChar == '<')  htmlText += "&lt;";
    else if (currentChar == '>')  htmlText += "&gt;";
    else if (currentChar == '"')  htmlText += "&quot;";
    else                          htmlText += currentChar;
  }

  return htmlText;
}


String currentModeLabel() {
  return (cfg.currentMode == Mode::Identify) ? "Identifizierung" : "Betrieb";
}

// =================================================
// Web Handlers
// =================================================

void handleRoot() {
  httpServer.sendHeader("Location", "/config");
  httpServer.send(302, "text/plain", "");
}

void handleConfigForm() {
  String html =
    "<h2>Temperaturlogger</h2>"
    "<p><a href='/rooms'>Räume (S1..SN)</a></p>"
    "<p><b>Modus aktuell:</b> " + currentModeLabel() + "</p>"
    "<p>ONE_WIRE_PIN ist fest auf GPIO4. LED auf GPIO2.</p>"
    "<form method='POST' action='/save'>"
    "Device-ID (auto, read-only):<br><input style='width:340px' value='" + toHtmlText(cfg.deviceId) + "' readonly><br><br>"
    "Alias Name (optional, editierbar):<br><input name='alias' style='width:340px' value='" + toHtmlText(cfg.alias) + "'><br><br>"

    "<hr>"
    "<b>Modus</b><br>"
    "<select name='mode' style='width:340px'>"
      "<option value='operate'"  + String(cfg.currentMode == Mode::Operate  ? " selected" : "") + ">Betrieb</option>"
      "<option value='identify'" + String(cfg.currentMode == Mode::Identify ? " selected" : "") + ">Identifizierung</option>"
    "</select><br><br>"

    "Identifizierung Refresh-Intervall (ms):<br>"
    "<input name='identifyIntervalMs' style='width:340px' value='" + String(cfg.identifyIntervalMs) + "'><br><br>"

    "<hr>"
    "<b>Betrieb</b><br>"
    "Mess-/Upload-Intervall (ms):<br><input name='operateIntervalMs' style='width:340px' value='" + String(cfg.operateIntervalMs) + "'><br><br>"
    "DS18B20 Temperatur-Auflösung (9–12 Bit):<br>"
    "<small>"
    "9 Bit = 0,5 °C Schritte, ~94 ms Messzeit<br>"
    "10 Bit = 0,25 °C Schritte, ~188 ms Messzeit<br>"
    "11 Bit = 0,125 °C Schritte, ~375 ms Messzeit<br>"
    "12 Bit = 0,0625 °C Schritte, ~750 ms Messzeit"
    "</small><br><br>"
    "<input name='tempResBits' style='width:340px' value='" + String(cfg.tempResBits) + "'><br><br>"

    "<hr>"
    "<b>InfluxDB</b><br>"
    "Influx-URL:<br><input name='url' style='width:340px' value='" + toHtmlText(cfg.url) + "'><br><br>"
    "Influx-OrgID:<br><input name='org' style='width:340px' value='" + toHtmlText(cfg.org) + "'><br><br>"
    "Daten-Depotname:<br><input name='bucket' style='width:340px' value='" + toHtmlText(cfg.bucket) + "'><br><br>"
    "Messtabellenname:<br><input name='measurement' style='width:340px' value='" + toHtmlText(cfg.measurement) + "'><br><br>"
    "Token:<br><input name='token' type='password' style='width:340px' value='" + toHtmlText(cfg.token) + "'><br><br>"
    "<button type='submit'>Save</button>"
    "</form><br>"
    "<form method='POST' action='/test'><button>Test InfluxDB Konfig</button></form><br>"
    "<form method='POST' action='/wifiReset' "
    "onsubmit=\"return confirm('WLAN-Credentials wirklich loeschen? Danach startet das Config-Portal.');\">"
    "<button>WiFi zuruecksetzen (nur WLAN)</button>"
    "</form><br>"
    "<form method='POST' action='/reboot'><button>Reboot</button></form>";

  httpServer.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  if (httpServer.hasArg("alias")) cfg.alias = httpServer.arg("alias");
  if (httpServer.hasArg("measurement")) cfg.measurement = httpServer.arg("measurement");

  if (httpServer.hasArg("operateIntervalMs"))
    cfg.operateIntervalMs = (unsigned long) httpServer.arg("operateIntervalMs").toInt();

  if (httpServer.hasArg("identifyIntervalMs"))
    cfg.identifyIntervalMs = (unsigned long) httpServer.arg("identifyIntervalMs").toInt();

  if (httpServer.hasArg("tempResBits"))
    cfg.tempResBits = httpServer.arg("tempResBits").toInt();

  if (httpServer.hasArg("mode")) {
    Mode setMode;
    if (parseModeFromInput(httpServer.arg("mode"), setMode)) {
      cfg.currentMode = setMode;
    }
  }

  if (httpServer.hasArg("url")) cfg.url = httpServer.arg("url");
  if (httpServer.hasArg("org")) cfg.org = httpServer.arg("org");
  if (httpServer.hasArg("bucket")) cfg.bucket = httpServer.arg("bucket");
  if (httpServer.hasArg("token")) cfg.token = httpServer.arg("token");

  // deviceId bleibt read-only
  forceDeviceIdFromMac();

  normalizeConfig();
  applyConfigToRuntime();

  if (!saveConfig()) {
    httpServer.send(500, "text/plain", "save failed");
    return;
  }

  httpServer.send(200, "text/plain", "saved");
}

void handleTest() {
  if (!influxConfigIsUsable()) {
    httpServer.send(400, "text/plain", "Konfiguration nicht vollständig (Influx-OrgID+Token angeben)");
    return;
  }

  if (!influxClient) initInfluxClient();

  bool ok = influxClient->validateConnection();
  httpServer.send(200, "text/plain", ok ? "influx ok" : influxClient->getLastErrorMessage());
}

void handleReboot() {
  httpServer.send(200, "text/plain", "rebooting");
  delay(300);
  ESP.restart();
}

void handleWifiReset() {
  // Antwort sofort schicken, bevor WLAN weg ist
  httpServer.send(200, "text/plain; charset=utf-8",
                  "WiFi credentials werden geloescht. Reboot...");

  delay(300);

  // WLAN-Creds aus NVS löschen + WiFi aus
  WiFi.disconnect(true, true);

  delay(200);
  ESP.restart();
}



void handleApiTemps() {
  int count = temperatureBus.getDeviceCount();
  if (count < 0) count = 0;
  if (count > MAX_ROOMS) count = MAX_ROOMS;

  temperatureBus.requestTemperatures();

  JsonDocument doc;
  doc["mode"] = modeToString(cfg.currentMode);
  doc["count"] = count;
  doc["ts_ms"] = (uint32_t)millis();

  JsonArray arr = doc["sensors"].to<JsonArray>();

  for (int i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["idx"] = i;
    o["sensor"] = sensorIdFromIndex(i);

    bool hasAddr = temperatureBus.getAddress(sensorAddress, i);
    o["rom"] = hasAddr ? romToString(sensorAddress) : "";

    float temperatureCelsius = NAN;
    bool ok = false;

    if (hasAddr) {
      temperatureCelsius = temperatureBus.getTempC(sensorAddress);
      ok = (temperatureCelsius != DEVICE_DISCONNECTED_C);
    }

    o["ok"] = ok;
    if (ok) o["tC"] = temperatureCelsius;
    else    o["err"] = "ERR";
  }

  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json; charset=utf-8", out);
}

void handleRoomsForm() {
  int count = temperatureBus.getDeviceCount();
  if (count < 0) count = 0;
  if (count > MAX_ROOMS) count = MAX_ROOMS;
  if ((int)cfg.rooms.size() < count) cfg.rooms.resize(count);

  const bool identify = (cfg.currentMode == Mode::Identify);

  String html = "<h2>Raumzuordnung</h2>"
                "<p><a href='/config'>Zurück</a></p>"
                "<p>Index-basiert: room[0]=S1, room[1]=S2, ...</p>";

  if (identify) {
    html += "<p><b>Identifizierung aktiv:</b> Temperaturen werden alle "
         + String(cfg.identifyIntervalMs)
         + " ms aktualisiert.</p>";
  } else {
    html += "<p><b>Betrieb aktiv:</b> Zuordnung kann geändert werden; Temperaturanzeige ist deaktiviert.</p>";
  }

  html += "<form method='POST' action='/saveRooms'>";

  for (int i = 0; i < count; i++) {
    String rom = "";
    if (temperatureBus.getAddress(sensorAddress, i)) rom = romToString(sensorAddress);

    html += "<b>" + sensorIdFromIndex(i) + "</b>";
    if (rom.length()) html += " <small>(ROM " + rom + ")</small>";

    if (identify) {
      html += " &nbsp; <span id='t" + String(i) + "' style='font-family:monospace'>...</span>";
    }

    html += "<br>";
    html += "<input name='room" + String(i+1) + "' style='width:340px' value='" + toHtmlText(cfg.rooms[i]) + "'>";
    html += "<br><br>";
  }

  html += "<button type='submit'>Save</button></form>";

  if (identify) {
    html += R"HTML(
<script>
(async function(){
  const intervalMs = )HTML" + String(cfg.identifyIntervalMs) + R"HTML(;

  function setText(id, txt){
    const el = document.getElementById(id);
    if (el) el.textContent = txt;
  }

  async function refresh(){
    try{
      const r = await fetch('/api/temps', {cache:'no-store'});
      if(!r.ok) throw new Error('http '+r.status);
      const j = await r.json();
      if(!j || !j.sensors) return;

      for(const s of j.sensors){
        const id = 't'+s.idx;
        if(s.ok){
          const v = (typeof s.tC === 'number') ? s.tC.toFixed(2) : String(s.tC);
          setText(id, 'TempC=' + v);
        }else{
          setText(id, 'TempC=ERR');
        }
      }
    }catch(e){
      // bewusst still: UI soll nicht spammen
    }
  }

  await refresh();
  setInterval(refresh, intervalMs);
})();
</script>
)HTML";
  }

  httpServer.send(200, "text/html; charset=utf-8", html);
}

void handleSaveRooms() {
  int count = temperatureBus.getDeviceCount();
  if (count < 0) count = 0;
  if (count > MAX_ROOMS) count = MAX_ROOMS;

  cfg.rooms.clear();
  cfg.rooms.resize(count);

  for (int i = 0; i < count; i++) {
    String key = "room" + String(i + 1);
    cfg.rooms[i] = httpServer.hasArg(key) ? httpServer.arg(key) : "";
  }

  forceDeviceIdFromMac();

  normalizeConfig();
  applyConfigToRuntime();

  if (!saveConfig()) {
    httpServer.send(500, "text/plain; charset=utf-8", "Das Speichern von Räumen ist fehlgeschlagen");
    return;
  }

  httpServer.send(200, "text/plain; charset=utf-8", "Räume gespeichert");
}

void registerRoutes() {
  httpServer.on("/", handleRoot);
  httpServer.on("/config", handleConfigForm);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.on("/test", HTTP_POST, handleTest);
  httpServer.on("/reboot", HTTP_POST, handleReboot);

  // NEU:
  httpServer.on("/wifiReset", HTTP_POST, handleWifiReset);

  httpServer.on("/rooms", handleRoomsForm);
  httpServer.on("/saveRooms", HTTP_POST, handleSaveRooms);

  httpServer.on("/api/temps", HTTP_GET, handleApiTemps);
}

// =================================================
// WiFiManager
// =================================================

void handleWifiResetOnBoot() {
  pinMode(0, INPUT_PULLUP);
  delay(50);

  // Taste gedrückt?
  if (digitalRead(0) == HIGH) return;

  unsigned long t0 = millis();
  while (digitalRead(0) == LOW) {
    if (millis() - t0 > 3000) {   // 3 Sekunden halten
      Serial.println("WiFi reset triggered");

      WiFi.disconnect(true, true); // WLAN-Credentials löschen
      delay(200);
      ESP.restart();
    }
    delay(10);
  }
}


bool shouldStartPortal() {
  pinMode(0, INPUT_PULLUP);
  delay(50);

  if (!cfgLoaded) return true;
  if (!influxConfigIsUsable() && cfg.currentMode == Mode::Operate) return true;

  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    if (digitalRead(0) == HIGH) return false;
    delay(10);
  }
  return (digitalRead(0) == LOW);
}

// =================================================
// Setup / Loop
// =================================================

void setup() {
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    setStatusLed(false);
  }

  Serial.begin(115200);
  delay(200);

  handleWifiResetOnBoot();

  Serial.println();
  Serial.println("Booting (operate/identify; identify shows live temps on /rooms) ...");

  cfgLoaded = loadConfig();
  if (!cfgLoaded) {
    cfg.url         = INFLUXDB_URL_DEFAULT;
    cfg.bucket      = INFLUXDB_BUCKET_DEFAULT;
    cfg.measurement = DEFAULT_MEASUREMENT;

    cfg.deviceId    = "";
    cfg.alias       = "";

    cfg.tempResBits        = DEFAULT_TEMP_RES;
    cfg.operateIntervalMs  = DEFAULT_INTERVAL_MS;
    cfg.identifyIntervalMs = DEFAULT_IDENT_INTERVAL_MS;

    cfg.org         = "";
    cfg.token       = "";

    cfg.currentMode = Mode::Operate;
    cfg.rooms.clear();

    normalizeConfig();
  }

  WiFi.mode(WIFI_STA);
  bool changed = forceDeviceIdFromMac();
  normalizeConfig();
  if (changed || !cfgLoaded) saveConfig();

  WiFi.setHostname(cfg.deviceId.c_str());

    WiFiManager wm;

  // Labels 1:1 wie WebUI
  WiFiManagerParameter p_measure("measurement", "Messtabellenname", cfg.measurement.c_str(), 32);

  char operateIntervalBuf[12];
  snprintf(operateIntervalBuf, sizeof(operateIntervalBuf), "%lu", cfg.operateIntervalMs);

  char resBuf[4];
  snprintf(resBuf, sizeof(resBuf), "%d", cfg.tempResBits);

  // WebUI: "Mess-/Upload-Intervall (ms)" und "DS18B20 Temperatur-Auflösung (9–12 Bit)"
  WiFiManagerParameter p_operateInterval("operateIntervalMs", "Mess-/Upload-Intervall (ms)", operateIntervalBuf, 11);
  WiFiManagerParameter p_res("tempResBits", "DS18B20 Temperatur-Auflösung (9–12 Bit)", resBuf, 3);

  // WebUI: "Influx-URL", "Influx-OrgID", "Bucket", "Token"
  WiFiManagerParameter p_url("url", "Influx-URL", cfg.url.c_str(), 128);
  WiFiManagerParameter p_org("org", "Influx-OrgID", cfg.org.c_str(), 64);
  WiFiManagerParameter p_bucket("bucket", "Daten-Depotname", cfg.bucket.c_str(), 64);
  WiFiManagerParameter p_token("token", "Token", cfg.token.c_str(), 160);

  wm.addParameter(&p_measure);
  wm.addParameter(&p_operateInterval);
  wm.addParameter(&p_res);
  wm.addParameter(&p_url);
  wm.addParameter(&p_org);
  wm.addParameter(&p_bucket);
  wm.addParameter(&p_token);


  const bool portal = shouldStartPortal();
  bool wifiOk = false;

  if (portal) {
    Serial.println("Starting Config Portal (AP: Temperaturlogger) ...");
    wifiOk = wm.startConfigPortal("AutoTemperaturlogger-AP");
  } else {
    wifiOk = wm.autoConnect("AutoTemperaturlogger-AP");
  }

  if (!wifiOk) {
    Serial.println("WiFi failed, restarting...");
    signalCycleFail();
    delay(1000);
    ESP.restart();
  }
  Serial.println("WiFi connected.");

  // Werte aus Portal übernehmen
  cfg.measurement       = p_measure.getValue();
  cfg.operateIntervalMs = (unsigned long) String(p_operateInterval.getValue()).toInt();
  cfg.tempResBits       = String(p_res.getValue()).toInt();
  cfg.url               = p_url.getValue();
  cfg.org               = p_org.getValue();
  cfg.bucket            = p_bucket.getValue();
  cfg.token             = p_token.getValue();

  // deviceId erneut aus STA-MAC erzwingen + speichern
  forceDeviceIdFromMac();
  normalizeConfig();
  saveConfig();

  // Zeit sync für TLS (Influx Cloud braucht gültige Zeit)
  Serial.print("TZ: ");
  Serial.println(TZ_INFO);
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  // Jetzt erst: Hardware/Services anwenden (Sensoren + Influx)
  applyConfigToRuntime();

  // Webserver starten
  registerRoutes();
  httpServer.begin();

  Serial.print("Web config: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/config");

  Serial.print("Rooms page: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/rooms");
}

void loop() {
  httpServer.handleClient();

  if (cfg.currentMode == Mode::Identify) return;

  const unsigned long nowMs = millis();
  if ((unsigned long)(nowMs - lastSampleMs) < cfg.operateIntervalMs) return;
  lastSampleMs = nowMs;

  if (!influxConfigIsUsable() || !influxClient) {
    Serial.println("Skip upload: Influx config missing (need org+token).");
    signalCycleFail();
    return;
  }

  int count = temperatureBus.getDeviceCount();
  if (count <= 0) {
    Serial.println("No sensors.");
    signalCycleFail();
    return;
  }
  if (count > MAX_ROOMS) count = MAX_ROOMS;

  temperatureBus.requestTemperatures();

  for (int sensorIndex = 0; sensorIndex < count; sensorIndex++) {
    if (!temperatureBus.getAddress(sensorAddress, sensorIndex)) continue;

    const float temperatureCelsius = temperatureBus.getTempC(sensorAddress);

    Point measurementPoint(cfg.measurement);
    measurementPoint.addTag("device", cfg.deviceId);
    if (cfg.alias.length() > 0) measurementPoint.addTag("alias", cfg.alias);
    measurementPoint.addTag("sensor", sensorIdFromIndex(sensorIndex));
    measurementPoint.addTag("room", roomForSensorIndex(sensorIndex));   // FIX: richtiger Funktionsname
    measurementPoint.addTag("rom", romToString(sensorAddress));
    measurementPoint.addField("value", temperatureCelsius);

    if (!influxClient->writePoint(measurementPoint)) {
      Serial.print("Buffer/write failed: ");
      Serial.println(influxClient->getLastErrorMessage());
    }
  }

  const bool ok = influxClient->flushBuffer();
  if (!ok) {
    Serial.print("Batch flush failed: ");
    Serial.println(influxClient->getLastErrorMessage());
    signalCycleFail();
  } else {
    signalCycleOk();
  }
}
