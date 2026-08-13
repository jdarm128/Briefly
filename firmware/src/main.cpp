// ===========================================================================
// Briefing Station - MVP firmware
// Board: LILYGO TTGO T-Display (ESP32) + SSD1306 OLED + CAP1188 touch +
//        LSM6DSO IMU (knock) + DHT20 temp/humidity + buzzer + LED + buttons
//
// What works out of the box (no CAP1188 wired yet? still fully usable):
//   * Wi-Fi + NTP clock on the color LCD, alarms stored on-chip (survive
//     reboot), buzzer alarm, DOUBLE-KNOCK the case to snooze (IMU tap int.)
//   * MQTT: renders "cards" pushed by the gateway on the OLED, sends events
//     (knock/keys/alarm actions) and temp/humidity telemetry back up.
//   * Onboard buttons: A (GPIO0) short = request update, B (GPIO35) short =
//     next card, either held ~1.2 s = dismiss alarm.
// Every peripheral is optional at runtime: if it isn't found on the I2C bus
// the firmware logs it and keeps going, so you can bring hardware up in
// stages (milestone M1 -> M3 in the blueprint).
// ===========================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_CAP1188.h>
#include <Adafruit_AHTX0.h>

#include "config.h"
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy firmware/include/secrets_example.h to firmware/include/secrets.h and edit it."
#endif

// ---------------------------------------------------------------- hardware --
TFT_eSPI          tft;
Adafruit_SSD1306  oled(128, 64, &Wire, -1);
Adafruit_CAP1188  cap;               // 8-pad capacitive touch
Adafruit_AHTX0    aht;               // DHT20 speaks the AHT20 protocol
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);
Preferences       prefs;

bool hasOled = false, hasCap = false, hasAht = false, hasImu = false;
uint8_t imuAddr = 0x6B;

// Colors for the T-Display (565)
#define C_AMBER  tft.color565(245, 197, 66)
#define C_CYAN   tft.color565(79, 195, 247)
#define C_MUT    tft.color565(154, 163, 173)

// ------------------------------------------------------------------- state --
struct Card { String id, title, l1, l2; };
Card cards[MAX_CARDS];
int  nCards = 0, curCard = 0;

struct Alarm { uint8_t hh = 0, mm = 0, days = 0x7F; bool on = false; };
Alarm alarms[MAX_ALARMS];

enum RingState { IDLE, RINGING };
RingState ring = IDLE;
time_t    snoozeUntil = 0;
int       lastFiredKey = -1;          // yday*10000 + hh*100 + mm, to fire once

volatile bool imuFlag = false;        // set by INT1 ISR on double-tap
uint32_t lastKnockMs = 0;
uint32_t lastActivityMs = 0;          // backlight timer
bool     backlightOn = true;

float lastTempF = NAN, lastRh = NAN;

SemaphoreHandle_t stateLock;          // guards cards/ring/alarm text for UI task

// ------------------------------------------------------- small utilities ----
static String fitStr(const String& s, int n = CARD_CHARS) {
  return (int)s.length() <= n ? s : s.substring(0, n);
}

static bool timeSynced() { return time(nullptr) > 1700000000; }

static void logLine(const char* tag, const String& msg) {
  Serial.printf("[%s] %s\n", tag, msg.c_str());
}

// ------------------------------------------------- LSM6DSO knock (AN5192) ---
// Double-tap recognition configured with raw register writes, following ST's
// application note AN5192, and routed to INT1 -> PIN_IMU_INT.
static bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}
static int imuRead(uint8_t reg) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)imuAddr, 1) != 1) return -1;
  return Wire.read();
}
void IRAM_ATTR onImuInt() { imuFlag = true; }

static bool imuInitDoubleTap() {
  const uint8_t addrs[2] = {0x6B, 0x6A};
  for (uint8_t a : addrs) {
    imuAddr = a;
    if (imuRead(0x0F) == 0x6C) {                 // WHO_AM_I
      bool ok = true;
      ok &= imuWrite(0x10, 0x60);  // CTRL1_XL: 417 Hz, +/-2 g
      ok &= imuWrite(0x56, 0x0E);  // TAP_CFG0: enable X/Y/Z tap
      ok &= imuWrite(0x57, 0x0C);  // TAP_CFG1: X threshold
      ok &= imuWrite(0x58, 0x8C);  // TAP_CFG2: enable interrupts + Y thr
      ok &= imuWrite(0x59, 0x0C);  // TAP_THS_6D: Z threshold
      ok &= imuWrite(0x5A, 0x7F);  // INT_DUR2: duration/quiet/shock windows
      ok &= imuWrite(0x5B, 0x80);  // WAKE_UP_THS: single+double tap mode
      ok &= imuWrite(0x5E, 0x08);  // MD1_CFG: route double-tap to INT1
      if (ok) {
        pinMode(PIN_IMU_INT, INPUT);
        attachInterrupt(digitalPinToInterrupt(PIN_IMU_INT), onImuInt, RISING);
      }
      return ok;
    }
  }
  return false;
}

// ------------------------------------------------------------ alarm store ---
// Stored in NVS as "hh:mm:daysmask:on;..." so alarms survive power loss.
static void alarmsSave() {
  String s;
  for (int i = 0; i < MAX_ALARMS; i++)
    s += String(alarms[i].hh) + ":" + String(alarms[i].mm) + ":" +
         String(alarms[i].days) + ":" + String(alarms[i].on ? 1 : 0) + ";";
  prefs.putString("alarms", s);
}
static void alarmsLoad() {
  String s = prefs.getString("alarms", "");
  int idx = 0, from = 0;
  while (idx < MAX_ALARMS) {
    int semi = s.indexOf(';', from);
    if (semi < 0) break;
    String part = s.substring(from, semi);
    from = semi + 1;
    int a = part.indexOf(':'), b = part.indexOf(':', a + 1), c = part.indexOf(':', b + 1);
    if (a < 0 || b < 0 || c < 0) break;
    alarms[idx].hh   = part.substring(0, a).toInt();
    alarms[idx].mm   = part.substring(a + 1, b).toInt();
    alarms[idx].days = part.substring(b + 1, c).toInt();
    alarms[idx].on   = part.substring(c + 1).toInt() == 1;
    idx++;
  }
}
static String nextAlarmText() {
  for (int i = 0; i < MAX_ALARMS; i++)
    if (alarms[i].on) {
      char b[16];
      snprintf(b, sizeof(b), "alarm %02d:%02d", alarms[i].hh, alarms[i].mm);
      return String(b);
    }
  return "no alarm";
}

// ------------------------------------------------------------ mqtt output ---
static void pubEvent(const char* type, const char* k = nullptr, const char* v = nullptr) {
  if (!mqtt.connected()) return;
  StaticJsonDocument<192> d;
  d["type"] = type;
  if (k && v) d[k] = v;
  char buf[192];
  serializeJson(d, buf, sizeof(buf));
  mqtt.publish(T_EVENT, buf);
}

static void pubTelemetry() {
  if (!mqtt.connected() || !hasAht) return;
  sensors_event_t hum, temp;
  aht.getEvent(&hum, &temp);
  lastTempF = temp.temperature * 9.0f / 5.0f + 32.0f;
  lastRh    = hum.relative_humidity;
  StaticJsonDocument<128> d;
  d["tempF"] = serialized(String(lastTempF, 1));
  d["rh"]    = serialized(String(lastRh, 0));
  char buf[128];
  serializeJson(d, buf, sizeof(buf));
  mqtt.publish(T_TELEMETRY, buf);
}

// ----------------------------------------------------------- ring control ---
static void wake() { lastActivityMs = millis(); }

static void startRing() {
  ring = RINGING;
  wake();
  digitalWrite(PIN_LED, HIGH);
  pubEvent("alarm", "action", "ringing");
  logLine("ALARM", "ringing");
}
static void stopRing(const char* how) {
  ring = IDLE;
  ledcWriteTone(BUZZER_LEDC_CH, 0);
  digitalWrite(PIN_LED, LOW);
  pubEvent("alarm", "action", how);
  logLine("ALARM", how);
}
static void snooze() {
  snoozeUntil = time(nullptr) + SNOOZE_SECONDS;
  stopRing("snoozed");
}

// ------------------------------------------------------------- mqtt input ---
static void handleCards(byte* payload, unsigned int len) {
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, payload, len)) { logLine("CARD", "bad JSON"); return; }
  JsonArray arr = doc["cards"].as<JsonArray>();
  if (arr.isNull()) return;
  xSemaphoreTake(stateLock, portMAX_DELAY);
  nCards = 0;
  for (JsonObject c : arr) {
    if (nCards >= MAX_CARDS) break;
    cards[nCards].id    = String((const char*)(c["id"]    | ""));
    cards[nCards].title = fitStr(String((const char*)(c["title"] | "")), 18);
    cards[nCards].l1    = fitStr(String((const char*)(c["line1"] | "")));
    cards[nCards].l2    = fitStr(String((const char*)(c["line2"] | "")));
    nCards++;
  }
  if (curCard >= nCards) curCard = 0;
  xSemaphoreGive(stateLock);
  logLine("CARD", String(nCards) + " cards received");
}

static void handleCommand(byte* payload, unsigned int len) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* type = doc["type"] | "";
  if (!strcmp(type, "set_alarm")) {
    int slot = doc["slot"] | 0;
    if (slot < 0 || slot >= MAX_ALARMS) slot = 0;
    int hh = 0, mm = 0;
    sscanf(doc["time"] | "07:00", "%d:%d", &hh, &mm);
    uint8_t mask = 0;
    JsonArray days = doc["days"].as<JsonArray>();
    if (days.isNull()) mask = 0x7F;
    else for (int d : days) if (d >= 0 && d <= 6) mask |= (1 << d);
    xSemaphoreTake(stateLock, portMAX_DELAY);
    alarms[slot].hh = hh; alarms[slot].mm = mm;
    alarms[slot].days = mask; alarms[slot].on = true;
    alarmsSave();
    xSemaphoreGive(stateLock);
    logLine("ALARM", "set " + String(hh) + ":" + String(mm));
    pubEvent("alarm", "action", "set");
  } else if (!strcmp(type, "clear_alarms")) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    for (int i = 0; i < MAX_ALARMS; i++) alarms[i].on = false;
    alarmsSave();
    xSemaphoreGive(stateLock);
    if (ring == RINGING) stopRing("dismissed");
    pubEvent("alarm", "action", "cleared");
  } else if (!strcmp(type, "beep")) {
    ledcWriteTone(BUZZER_LEDC_CH, 1047); delay(150);
    ledcWriteTone(BUZZER_LEDC_CH, 0);
  }
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (!strcmp(topic, T_CARDS))   handleCards(payload, len);
  if (!strcmp(topic, T_COMMAND)) handleCommand(payload, len);
}

// ------------------------------------------------ wifi / mqtt / time mgmt ---
uint32_t lastWifiTry = 0, lastMqttTry = 0;
bool timeConfigured = false;

static void manageWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeConfigured) {
      configTzTime(TZ_INFO, NTP_1, NTP_2);
      timeConfigured = true;
      logLine("WIFI", "connected, ip " + WiFi.localIP().toString());
    }
    return;
  }
  if (millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    logLine("WIFI", "connecting to " WIFI_SSID " ...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

static void manageMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - lastMqttTry > 5000) {
    lastMqttTry = millis();
    String cid = "station-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSWD)) {
      mqtt.subscribe(T_CARDS);
      mqtt.subscribe(T_COMMAND);
      StaticJsonDocument<256> d;
      d["type"] = "boot";
      d["oled"] = hasOled; d["touch"] = hasCap; d["imu"] = hasImu; d["dht20"] = hasAht;
      char buf[256];
      serializeJson(d, buf, sizeof(buf));
      mqtt.publish(T_EVENT, buf);
      logLine("MQTT", "connected");
    } else {
      logLine("MQTT", "connect failed rc=" + String(mqtt.state()));
    }
  }
}

// ------------------------------------------------------------------ inputs --
struct Btn { 
  uint8_t pin; 
  bool wasDown = false; 
  uint32_t downAt = 0; 
  bool longFired = false; 
  Btn(uint8_t p) { pin = p; }
};
Btn btnA(PIN_BTN_A);
Btn btnB(PIN_BTN_B);

static void onShortPress(char which) {
  wake();
  if (ring == RINGING) { snooze(); return; }         // any tap while ringing = snooze
  if (which == 'A') { pubEvent("key", "id", "update"); logLine("KEY", "update requested"); }
  if (which == 'B') {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    if (nCards > 0) curCard = (curCard + 1) % nCards;
    xSemaphoreGive(stateLock);
  }
}
static void onLongPress() {
  wake();
  if (ring == RINGING) stopRing("dismissed");
}

static void pollButton(Btn& b, char which) {
  bool down = digitalRead(b.pin) == LOW;
  if (down && !b.wasDown) { b.downAt = millis(); b.longFired = false; }
  if (down && !b.longFired && millis() - b.downAt > LONGPRESS_MS) { b.longFired = true; onLongPress(); }
  if (!down && b.wasDown && !b.longFired && millis() - b.downAt > 30) onShortPress(which);
  b.wasDown = down;
}

uint8_t prevTouch = 0;
uint32_t lastTouchPoll = 0;
static void pollTouch() {
  if (!hasCap || millis() - lastTouchPoll < 60) return;
  lastTouchPoll = millis();
  uint8_t t = cap.touched();
  uint8_t rising = t & ~prevTouch;
  prevTouch = t;
  if (!rising) return;
  wake();
  if (ring == RINGING) { stopRing("dismissed"); return; }  // any pad kills a ringing alarm
  if (rising & 0x01) {                                     // pad 1: next card
    xSemaphoreTake(stateLock, portMAX_DELAY);
    if (nCards > 0) curCard = (curCard + 1) % nCards;
    xSemaphoreGive(stateLock);
  }
  if (rising & 0x02) pubEvent("key", "id", "update");      // pad 2: refresh brief
}

static void handleKnock() {
  if (millis() - lastKnockMs < KNOCK_COOLDOWN_MS) return;
  lastKnockMs = millis();
  wake();
  if (ring == RINGING) { snooze(); return; }
  pubEvent("knock");            // gateway treats a knock as "refresh the brief"
  logLine("KNOCK", "double-tap");
}

// ---------------------------------------------------------------- alarms ----
static void checkAlarms() {
  if (!timeSynced()) return;
  time_t now = time(nullptr);
  if (snoozeUntil && now >= snoozeUntil) { snoozeUntil = 0; startRing(); return; }
  struct tm ti;
  localtime_r(&now, &ti);
  int key = ti.tm_yday * 10000 + ti.tm_hour * 100 + ti.tm_min;
  if (key == lastFiredKey) return;
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].on && (alarms[i].days & (1 << ti.tm_wday)) &&
        alarms[i].hh == ti.tm_hour && alarms[i].mm == ti.tm_min) {
      lastFiredKey = key;
      if (ring != RINGING) startRing();
    }
  }
}

static void runMelody() {
  if (ring != RINGING) return;
  // simple two-tone pattern, non-blocking
  uint32_t phase = (millis() / 250) % 4;
  static uint32_t lastPhase = 99;
  if (phase == lastPhase) return;
  lastPhase = phase;
  if (phase == 0) ledcWriteTone(BUZZER_LEDC_CH, 880);
  else if (phase == 1) ledcWriteTone(BUZZER_LEDC_CH, 1320);
  else if (phase == 2) ledcWriteTone(BUZZER_LEDC_CH, 880);
  else ledcWriteTone(BUZZER_LEDC_CH, 0);
  digitalWrite(PIN_LED, phase % 2 ? HIGH : LOW);
}

// -------------------------------------------------------------------- UI ----
static void drawOled() {
  if (!hasOled) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  if (ring == RINGING) {
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(10, 8);  oled.print("ALARM!");
    oled.setTextSize(1);
    oled.setCursor(0, 34);  oled.print("knock 2x = snooze");
    oled.setCursor(0, 46);  oled.print("hold btn = dismiss");
  } else if (nCards == 0) {
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);   oled.print("Briefing Station");
    oled.setCursor(0, 16);  oled.print(mqtt.connected() ? "waiting for cards" : "waiting for MQTT");
    oled.setCursor(0, 28);  oled.print("btn A / knock =");
    oled.setCursor(0, 40);  oled.print("request update");
  } else {
    Card c = cards[curCard];
    oled.fillRect(0, 0, 128, 12, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(2, 2);   oled.print(c.title);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 18);  oled.print(c.l1);
    oled.setCursor(0, 32);  oled.print(c.l2);
    oled.setCursor(0, 54);
    oled.print(String(curCard + 1) + "/" + String(nCards));
    if (!isnan(lastTempF)) {
      oled.setCursor(60, 54);
      oled.print(String(lastTempF, 0) + "F " + String(lastRh, 0) + "%");
    }
  }
  oled.display();
}

static void drawTft() {
  static String lastClock = "", lastFoot = "", lastSub = "";
  struct tm ti;
  bool haveTime = timeSynced() && getLocalTime(&ti, 10);

  char clockBuf[8] = "--:--";
  char dateBuf[24] = "syncing time...";
  if (haveTime) {
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    strftime(dateBuf, sizeof(dateBuf), "%a %b %d", &ti);
  }

  if (ring == RINGING) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_AMBER, TFT_BLACK);
    tft.drawString("ALARM", 120, 45, 6);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("knock 2x to snooze", 120, 100, 2);
    lastClock = "";                       // force full redraw after dismissal
    return;
  }

  String foot = String(WiFi.status() == WL_CONNECTED ? "wifi" : "----") +
                (mqtt.connected() ? " mqtt " : " ---- ") + nextAlarmText();
  String sub  = (nCards > 0) ? cards[0].l1 : String(dateBuf);

  if (lastClock == clockBuf && lastFoot == foot && lastSub == sub) return;
  if (lastClock == "") tft.fillScreen(TFT_BLACK);
  lastClock = clockBuf; lastFoot = foot; lastSub = sub;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_MUT, TFT_BLACK);
  tft.setTextPadding(238);
  tft.drawString(dateBuf, 120, 6, 2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(238);
  tft.drawString(clockBuf, 120, 60, 7);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_CYAN, TFT_BLACK);
  tft.setTextPadding(238);
  tft.drawString(sub, 120, 100, 2);

  tft.setTextColor(C_MUT, TFT_BLACK);
  tft.setTextPadding(238);
  tft.drawString(foot, 120, 120, 2);
}

static void uiTask(void*) {
  for (;;) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    drawTft();
    drawOled();
    xSemaphoreGive(stateLock);
    // backlight management
    bool wantBl = (ring == RINGING) || (millis() - lastActivityMs < BACKLIGHT_MS);
    if (wantBl != backlightOn) {
      backlightOn = wantBl;
      digitalWrite(PIN_TFT_BL, wantBl ? HIGH : LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ------------------------------------------------------------------ setup ---
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Briefing Station boot ===");

  stateLock = xSemaphoreCreateMutex();

  pinMode(PIN_LED, OUTPUT);       digitalWrite(PIN_LED, LOW);
  pinMode(PIN_TFT_BL, OUTPUT);    digitalWrite(PIN_TFT_BL, HIGH);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT);      // GPIO35: input-only, external pull-up on board
  ledcSetup(BUZZER_LEDC_CH, 2000, 10);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Briefing Station", 120, 55, 4);
  tft.setTextColor(C_MUT, TFT_BLACK);
  tft.drawString("booting...", 120, 90, 2);

  hasOled = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  logLine("BOOT", hasOled ? "OLED  0x3C  [OK]" : "OLED  0x3C  [--]");
  if (hasOled) { oled.clearDisplay(); oled.display(); }

  hasCap = cap.begin(0x29) || cap.begin(0x28);
  logLine("BOOT", hasCap ? "CAP1188      [OK]" : "CAP1188      [--] (touch pads optional)");

  hasAht = aht.begin();
  logLine("BOOT", hasAht ? "DHT20 0x38   [OK]" : "DHT20 0x38   [--]");

  hasImu = imuInitDoubleTap();
  logLine("BOOT", hasImu ? "LSM6DSO knock[OK]" : "LSM6DSO      [--] (knock disabled)");

  prefs.begin("station", false);
  alarmsLoad();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(4096);            // card JSON is bigger than the 256 default
  mqtt.setCallback(mqttCallback);

  wake();
  xTaskCreatePinnedToCore(uiTask, "ui", 6144, nullptr, 1, nullptr, 1);
  logLine("BOOT", "ready");
}

// ------------------------------------------------------------------- loop ---
uint32_t lastAlarmCheck = 0, lastTelemetry = 0, lastBeatLed = 0;

void loop() {
  manageWifi();
  manageMqtt();

  pollButton(btnA, 'A');
  pollButton(btnB, 'B');
  pollTouch();

  if (imuFlag) { imuFlag = false; if (hasImu) { imuRead(0x1C); handleKnock(); } }

  if (millis() - lastAlarmCheck > 1000) { lastAlarmCheck = millis(); checkAlarms(); }
  runMelody();

  if (millis() - lastTelemetry > TELEMETRY_MS) { lastTelemetry = millis(); pubTelemetry(); }

  delay(10);
}
