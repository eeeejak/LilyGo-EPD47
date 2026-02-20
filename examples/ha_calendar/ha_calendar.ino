/**
 * Simple Home Assistant calendar display
 * Based on LilyGo EPD47 examples, but stripped down:
 * - No web server
 * - No file upload
 * - Just fetches two HA sensors and prints them.
 */

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> Tools -> PSRAM -> OPI PSRAM"
#endif

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include "epd_driver.h"
#include "firasans.h"
#include "secrets.h"

// ---------- CONFIG ----------
// WiFi (loaded from secrets.h, which is not committed to git)
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Home Assistant (host/port loaded from secrets.h)
const char* HA_HOST  = HA_HOST_ADDR;   // <-- your HA IP
const uint16_t HA_PORT = HA_PORT_NUM;

// Long-lived access token from HA (from secrets.h)
const char* HA_TOKEN = HA_TOKEN_VALUE;

// HA entities we created
const char* ENTITY_LINE1 = "sensor.time"; // "sensor.desk_calendar_line_1";
const char* ENTITY_LINE2 = "sensor.time"; // "sensor.desk_calendar_line_2";
const char* ENTITY_CLOCK = "sensor.time"; // "sensor.desk_clock_line";

// Update intervals (ms)
const unsigned long CAL_UPDATE_INTERVAL_MS = 60UL * 60UL * 1000UL; // 1 hour
const unsigned long CLOCK_UPDATE_INTERVAL_MS = 60UL * 1000UL;        // 1 minute

// ---------- Layout (reuse from example) ----------
const Rect_t line1Area = {
    .x = 0,
    .y = 387,
    .width = 960,
    .height = 51,
};
const Rect_t line2Area = {
    .x = 0,
    .y = 438,
    .width = 960,
    .height = 51,
};

const Rect_t clockArea = {
    .x = 0,
    .y = 330,      // adjust as needed
    .width = 100,
    .height = 51,
};

// ---------- Globals ----------
unsigned long lastCalUpdate   = 0;
unsigned long lastClockUpdate = 0;

// Last drawn text for change detection
String lastClockText;
String lastCalLine1Text;
String lastCalLine2Text;

// ---------- WiFi helpers ----------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed");
  }
}

// ---------- HA REST helper ----------
String fetchSensorState(const char* entity_id) {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    return "WiFi error";
  }

  HTTPClient http;
  String url = String("http://") + HA_HOST + ":" + HA_PORT + "/api/states/" + entity_id;

  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return "HTTP " + String(httpCode);
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return "JSON error";
  }

  const char* state = doc["state"];
  if (!state) {
    return "No state";
  }

  return String(state);
}

// ---------- Drawing helpers ----------
void drawTextInArea(const Rect_t& area, const char* text) {
  int32_t cursor_x = area.x;
  int32_t cursor_y = area.y + FiraSans.advance_y + FiraSans.descender;

  // Clear only this area (partial refresh)
  epd_clear_area(area);

  // Draw using the same font + function the example uses
  writeln((GFXfont *)&FiraSans, text, &cursor_x, &cursor_y, NULL);
}

void drawInitialScreen() {
  // One-time full clear on boot so the panel starts in a known state
  epd_poweron();
  epd_clear();
  epd_poweroff();
}

void drawCalendarScreen(const String& clock, const String& line1, const String& line2) {
  // If nothing changed, don’t bother redrawing this set
  if (clock == lastClockText && line1 == lastCalLine1Text && line2 == lastCalLine2Text) {
    return;
  }

  lastClockText    = clock;
  lastCalLine1Text = line1;
  lastCalLine2Text = line2;

  epd_poweron();

  // Clock line (partial refresh)
  drawTextInArea(clockArea, clock.c_str());

  // Time line (partial refresh)
  drawTextInArea(line1Area, line1.c_str());

  // Title line (partial refresh)
  drawTextInArea(line2Area, line2.c_str());

  epd_poweroff();
}

// Clock-only partial refresh
void drawClockOnly(const String& clock) {
  // Skip if the clock text hasn’t changed
  if (clock == lastClockText) {
    return;
  }

  lastClockText = clock;

  epd_poweron();
  drawTextInArea(clockArea, clock.c_str());
  epd_poweroff();
}

// ---------- Arduino lifecycle ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nEPD47 Home Assistant Calendar");

  // Init display
  epd_init();
  // One-time clear
  drawInitialScreen();

  // Initial WiFi connect
  connectWiFi();
}

void loop() {
  unsigned long now = millis();

  // Calendar + event + clock (full set) every CAL_UPDATE_INTERVAL_MS
  if (lastCalUpdate == 0 || (now - lastCalUpdate) > CAL_UPDATE_INTERVAL_MS) {
    lastCalUpdate = now;

    String clock = fetchSensorState(ENTITY_CLOCK);
    String l1    = fetchSensorState(ENTITY_LINE1);
    String l2    = fetchSensorState(ENTITY_LINE2);

    Serial.println("[HA] FULL Clock: " + clock);
    Serial.println("[HA] FULL Line1: " + l1);
    Serial.println("[HA] FULL Line2: " + l2);

    drawCalendarScreen(clock, l1, l2);

    // We also just updated the clock
    lastClockUpdate = now;
  }

  // Clock-only update every CLOCK_UPDATE_INTERVAL_MS
  if (lastClockUpdate == 0 || (now - lastClockUpdate) > CLOCK_UPDATE_INTERVAL_MS) {
    lastClockUpdate = now;

    String clock = fetchSensorState(ENTITY_CLOCK);
    Serial.println("[HA] CLOCK-ONLY: " + clock);

    drawClockOnly(clock);
  }

  delay(1000);
}