#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM: Arduino IDE -> Tools -> PSRAM -> OPI PSRAM"
#endif

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <Wire.h>
#include "epd_driver.h"
#include "firasans.h"
#include "secrets.h" 
#include <TouchDrvGT911.hpp> // Installer "SensorLib" via le gestionnaire de bibliothèques
#include "utilities.h"       // Nécessaire pour les pins BOARD_SDA, BOARD_SCL du S3

// ---------- CONFIG ----------
#define GRID_COLS 4
#define GRID_ROWS 4
#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 540
#define MARGIN 10

uint8_t *framebuffer;
TouchDrvGT911 touch;
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 5UL * 60UL * 1000UL;

struct Sensor {
    const char* title;
    const char* state_entity;   // Entité pour lire l'état
    const char* action_service; // Service HA (ex: "switch/toggle", "light/toggle", "button/press")
    const char* action_entity;  // Entité à commander (si différente)
};

// LISTE DES 16 CASES (Configurables)
Sensor sensors[16] = {
    {"Salon", "sensor.temp_salon", "switch/toggle", "switch.lumiere_salon"},
    {"Bureau", "sensor.temp_bureau", "light/toggle", "light.bureau"},
    {"Vent", "sensor.wind_speed", "button/press", "button.update_weather"},
    {"Volet", "sensor.v_position", "cover/stop", "cover.volet_salon"},
    // ... Complétez les 16 cases sur le même modèle ...
    {"Update", "sensor.time", "homeassistant/update_entity", "sensor.time"},
    {"Test 6", "sensor.time", "", ""}, {"Test 7", "sensor.time", "", ""}, {"Test 8", "sensor.time", "", ""},
    {"Test 9", "sensor.time", "", ""}, {"Test 10", "sensor.time", "", ""}, {"Test 11", "sensor.time", "", ""}, {"Test 12", "sensor.time", "", ""},
    {"Test 13", "sensor.time", "", ""}, {"Test 14", "sensor.time", "", ""}, {"Test 15", "sensor.time", "", ""}, {"OFF", "sensor.time", "script/turn_on", "script.all_off"}
};

// ---------- FONCTIONS HOME ASSISTANT ----------

void sendHAAction(int index) {
    if (strlen(sensors[index].action_service) == 0) return;
    
    HTTPClient http;
    String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM + "/api/services/" + sensors[index].action_service;
    
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"entity_id\":\"" + String(sensors[index].action_entity) + "\"}";
    int httpCode = http.POST(payload);
    Serial.printf("[HA] Action %s sur %s -> Code: %d\n", sensors[index].action_service, sensors[index].action_entity, httpCode);
    http.end();
}

String fetchSensorState(const char* entity_id) {
    HTTPClient http;
    String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM + "/api/states/" + entity_id;
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
    
    int httpCode = http.GET();
    if (httpCode != 200) { http.end(); return "Err"; }
    
    JsonDocument doc;
    deserializeJson(doc, http.getString());
    http.end();
    return doc["state"].as<String>();
}

// ---------- DESSIN ----------

void drawCell(int index, String state, bool inverted = false) {
    int cellW = SCREEN_WIDTH / GRID_COLS;
    int cellH = SCREEN_HEIGHT / GRID_ROWS;
    int x = (index % GRID_COLS) * cellW + (MARGIN/2);
    int y = (index / GRID_COLS) * cellH + (MARGIN/2);
    int w = cellW - MARGIN;
    int h = cellH - MARGIN;

    uint8_t bgColor = inverted ? 0 : 255; // 0 = Noir, 255 = Blanc (approximatif)
    uint8_t fgColor = inverted ? 255 : 0;

    // Fond de la case
    Rect_t area = { .x = x, .y = y, .width = (uint32_t)w, .height = (uint32_t)h };
    epd_fill_rect(x, y, w, h, bgColor, framebuffer);
    epd_draw_rect(x, y, w, h, 0, framebuffer); // Bordure toujours noire

    int tx = x + 10;
    int ty = y + 35;
    writeln((GFXfont *)&FiraSans, sensors[index].title, &tx, &ty, framebuffer);

    tx = x + 10;
    ty = y + 95;
    writeln((GFXfont *)&FiraSans, state.c_str(), &tx, &ty, framebuffer);
}

void flashCell(int index) {
    epd_poweron();
    drawCell(index, "...", true); // Version inversée
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    delay(200); // Temps du flash
    drawCell(index, "OK", false); // Retour normal
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

void updateDashboard() {
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    for (int i = 0; i < 16; i++) {
        drawCell(i, fetchSensorState(sensors[i].state_entity));
    }
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

// ---------- SETUP & LOOP ----------

void setup() {
    Serial.begin(115200);
    epd_init();
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);

    // Initialisation Tactile S3
    Wire.begin(BOARD_SDA, BOARD_SCL);
    touch.setPins(-1, TOUCH_INT);
    if (!touch.begin(Wire, 0x5D, BOARD_SDA, BOARD_SCL)) { // Adresse souvent 0x5D ou 0x14
        Serial.println("GT911 non trouvé !");
    }
    touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(false, true);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(500);

    updateDashboard();
}

void loop() {
    int16_t tx, ty;
    if (touch.getPoint(&tx, &ty)) {
        // Déterminer la case cliquée
        int col = tx / (SCREEN_WIDTH / GRID_COLS);
        int row = ty / (SCREEN_HEIGHT / GRID_ROWS);
        int index = row * GRID_COLS + col;

        if (index >= 0 && index < 16) {
            Serial.printf("Clic sur case %d : %s\n", index, sensors[index].title);
            flashCell(index);   // Feedback visuel
            sendHAAction(index); // Commande HA
            delay(500);          // Anti-rebond
        }
    }

    if (millis() - lastUpdate > UPDATE_INTERVAL) {
        lastUpdate = millis();
        updateDashboard();
    }
}