#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM: Arduino IDE -> Tools -> PSRAM -> OPI PSRAM"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "epd_driver.h"
#include "firasans.h"
#include "secrets.h" 
#include <TouchDrvGT911.hpp>
#include "utilities.h"

// ---------- CONFIGURATION DE LA GRILLE ----------
#define GRID_COLS 4
#define GRID_ROWS 4
#define NUM_SENSORS (GRID_COLS * GRID_ROWS)

#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 540
#define MARGIN 12
#define HEADER_H 60  // Passé à 60 pour avoir des hauteurs de cases entières (540-60 = 480 / 4 = 120px)

// Timers
const unsigned long SENSOR_INTERVAL = 1UL * 60UL * 1000UL;       // 1 minute
const unsigned long FULL_REFRESH_INTERVAL = 30UL * 60UL * 1000UL; // 30 minutes
unsigned long lastSensorUpdate = 0;
unsigned long lastFullRefresh = 0;

uint8_t *framebuffer = NULL;
TouchDrvGT911 touch;

struct Sensor {
    const char* title;
    const char* state_entity;
    const char* action_service;
    const char* action_entity;
    const char* sstitre;
    const char* color;
};

Sensor sensors[NUM_SENSORS] = {
    {"I'm going", "input_select.mode", "select", "out", "..OUT", ""},
    {"Bureau", "sensor.temperature_bureau", "light/toggle", "light.bureau", "", ""},
    {"Cuisine", "sensor.temperature_cuisine", "switch/toggle", "switch.cafetiere", "", ""},
    {"Extérieur", "sensor.outdoor_temp", "", "", "", ""},
    {"Good..", "sensor.humidity", "", "", "..Night !", ""},
    {"VMC", "sensor.vmc_status", "fan/toggle", "fan.vmc", "", ""},
    {"Solaire", "sensor.pv_power", "", "", "", ""},
    {"Batterie", "sensor.battery_level", "", "", "", ""},
    {"Garage", "binary_sensor.garage", "cover/toggle", "cover.garage", "", ""},
    {"Portail", "binary_sensor.portail", "switch/toggle", "switch.portail", "", ""},
    {"Eau", "sensor.water_meter", "", "", "", ""},
    {"Gaz", "sensor.gas_meter", "", "", "", ""},
    {"I'm going", "sensor.power_usage", "", "", "", ""},
    {"Mode", "input_select.house_mode", "input_select/select_next", "input_select.house_mode", "", ""},
    {"Alarme", "alarm_control_panel.maison", "alarm_control_panel/alarm_arm_home", "alarm_control_panel.maison", "", ""},
    {"Refresh", "sensor.time", "homeassistant/update_entity", "sensor.time", "", ""}
};

const char* headerTitles[] = {"CLIMAT", "ÉNERGIE", "ACCÈS", "Lights"};
String statesCache[NUM_SENSORS];

// ---------- REQUÊTES ASYNCHRONES (FreeRTOS) ----------

struct HARequest { String service; String entity; };

void haTask(void *pvParameters) {
    HARequest* req = (HARequest*)pvParameters;
    if (req->service.length() > 0) {
        HTTPClient http;
        String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM + "/api/services/" + req->service;
        http.begin(url);
        http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
        http.addHeader("Content-Type", "application/json");
        http.POST("{\"entity_id\":\"" + req->entity + "\"}");
        http.end();
    }
    delete req; 
    vTaskDelete(NULL); 
}

void sendHAActionAsync(int index) {
    if (strlen(sensors[index].action_service) == 0) return;
    HARequest* req = new HARequest{sensors[index].action_service, sensors[index].action_entity};
    xTaskCreate(haTask, "HA_Task", 4096, req, 1, NULL); // Lance la requête sans bloquer le code
}

// ---------- HELPERS HA ----------

String fetchSensorState(const char* entity_id) {
    if (WiFi.status() != WL_CONNECTED) return "WiFi?";
    HTTPClient http;
    String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM + "/api/states/" + entity_id;
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
    if (http.GET() == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        http.end();
        String s = doc["state"].as<String>();
        if (s == "on") return "OUI";
        if (s == "off") return "NON";
        if (s == "unavailable") return "---";
        return s;
    }
    http.end();
    return "-";
}

// ---------- DESSIN & PARTIAL REFRESH ----------

// Calcule la zone exacte d'un bouton
Rect_t getCellArea(int index) {
    int cellW = SCREEN_WIDTH / GRID_COLS;
    int cellH = (SCREEN_HEIGHT - HEADER_H) / GRID_ROWS;
    int col = index % GRID_COLS;
    int row = index / GRID_COLS;
    return (Rect_t){ .x = col * cellW, .y = HEADER_H + (row * cellH), .width = cellW, .height = cellH };
}

void drawHeaderToBuffer() {
    epd_fill_rect(0, 0, SCREEN_WIDTH, HEADER_H, 0xDD, framebuffer); // Gris clair
    epd_draw_line(0, HEADER_H-1, SCREEN_WIDTH, HEADER_H-1, 0, framebuffer);

    int sectionW = SCREEN_WIDTH / GRID_COLS;
    for (int i = 0; i < GRID_COLS; i++) {
        int tx = (i * sectionW) + (sectionW / 4) - 10;
        int ty = 40;
        String title = (i < (sizeof(headerTitles)/sizeof(headerTitles[0]))) ? headerTitles[i] : "ZONE";
        writeln((GFXfont *)&FiraSans, title.c_str(), &tx, &ty, framebuffer);
        if (i > 0) epd_draw_line(i * sectionW, 0, i * sectionW, HEADER_H, 0, framebuffer);
    }
}

void drawCellToBuffer(int index, String state, bool inverted = false) {
    Rect_t area = getCellArea(index);
    
    // Nettoyage précis de la case dans le buffer
    uint8_t bg = inverted ? 0x00 : 0xFF; 
    epd_fill_rect(area.x, area.y, area.width, area.height, bg, framebuffer);
    
    // Bordure
    int w = area.width - MARGIN;
    int h = area.height - MARGIN;
    int cx = area.x + (MARGIN / 2);
    int cy = area.y + (MARGIN / 2);
    epd_draw_rect(cx, cy, w, h, 0, framebuffer);

    // Titre (en gris 0x66 si non cliqué)
    int tx = cx + 10;
    int ty = cy + 35;
    writeln((GFXfont *)&FiraSans, sensors[index].title, &tx, &ty, framebuffer);

    // Valeur
    tx = cx + 15;
    ty = cy + 90;
    writeln((GFXfont *)&FiraSans, state.c_str(), &tx, &ty, framebuffer);
}

// Envoie UNIQUEMENT la zone du bouton à l'écran (Partial Refresh ciblé)
void pushCellToScreen(int index) {
    epd_poweron();
    epd_draw_grayscale_image(getCellArea(index), framebuffer);
    epd_poweroff();
}

// ---------- LOGIQUE DE MISE À JOUR ----------

// Le gros refresh qui nettoie l'écran (toutes les 30 min)
void performFullRefresh() {
    Serial.println("[TIMER] Full Screen Refresh (30 min)");
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    drawHeaderToBuffer();
    
    for (int i = 0; i < NUM_SENSORS; i++) {
        statesCache[i] = fetchSensorState(sensors[i].state_entity);
        drawCellToBuffer(i, statesCache[i], false);
    }
    
    epd_poweron();
    epd_clear(); // Nettoyage physique de la dalle e-ink
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
    
    lastFullRefresh = millis();
    lastSensorUpdate = millis();
}

// Le petit refresh qui vérifie les états (toutes les minutes)
void performSensorUpdate() {
    Serial.println("[TIMER] Sensor Data Update (1 min)");
    for (int i = 0; i < NUM_SENSORS; i++) {
        String newState = fetchSensorState(sensors[i].state_entity);
        if (newState != statesCache[i]) {
            statesCache[i] = newState;
            drawCellToBuffer(i, newState, false);
            pushCellToScreen(i); // Met à jour uniquement la case concernée
        }
    }
    lastSensorUpdate = millis();
}

// ---------- GESTION TACTILE ----------

void handleTouch(int16_t tx, int16_t ty) {
    if (ty < HEADER_H) return; // Ignore le bandeau gris

    int col = tx / (SCREEN_WIDTH / GRID_COLS);
    int row = (ty - HEADER_H) / ((SCREEN_HEIGHT - HEADER_H) / GRID_ROWS);
    int index = row * GRID_COLS + col;

    if (index >= 0 && index < NUM_SENSORS) {
        Serial.printf("Clic sur le bouton %d\n", index);

        // 1. Affiche le bouton en NOIR (Partial refresh immédiat)
        drawCellToBuffer(index, "...", true);
        pushCellToScreen(index);

        // 2. Envoie la requête HA en arrière-plan (non bloquant)
        sendHAActionAsync(index);

        // 3. Attend exactement 1 seconde
        delay(1000);

        // 4. Récupère le nouvel état, remet en blanc, et push à l'écran
        statesCache[index] = fetchSensorState(sensors[index].state_entity);
        drawCellToBuffer(index, statesCache[index], false);
        pushCellToScreen(index);
    }
}

// ---------- ARDUINO ----------

void setup() {
    Serial.begin(115200);
    epd_init();
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    
    Wire.begin(BOARD_SDA, BOARD_SCL);
    touch.setPins(-1, TOUCH_INT);
    if (touch.begin(Wire, 0x5D, BOARD_SDA, BOARD_SCL)) {
        touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(false, true);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    
    performFullRefresh(); // Premier affichage
}

void loop() {
    int16_t tx, ty;
    if (touch.getPoint(&tx, &ty)) {
        handleTouch(tx, ty);
    }

    unsigned long currentMillis = millis();

    if (currentMillis - lastFullRefresh >= FULL_REFRESH_INTERVAL) {
        performFullRefresh();
    } 
    else if (currentMillis - lastSensorUpdate >= SENSOR_INTERVAL) {
        performSensorUpdate();
    }

    delay(20); 
}