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

// ═══════════════════════════════════════════════════
//  CONFIGURATION GRILLE — seuls ces deux chiffres
//  à modifier pour changer la disposition
// ═══════════════════════════════════════════════════
#define GRID_COLS 4
#define GRID_ROWS 4

#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 540
#define MARGIN        12
#define HEADER_H      60   // 540-60 = 480 → 480/GRID_ROWS doit être entier

// ─── Timers ──────────────────────────────────────
const unsigned long SENSOR_INTERVAL      = 1UL  * 60UL * 1000UL;  // 1 min
const unsigned long FULL_REFRESH_INTERVAL = 30UL * 60UL * 1000UL; // 30 min
unsigned long lastSensorUpdate = 0;
unsigned long lastFullRefresh  = 0;

uint8_t *framebuffer = NULL;
TouchDrvGT911 touch;

// ═══════════════════════════════════════════════════
//  STRUCTURE CAPTEUR / BOUTON
// ═══════════════════════════════════════════════════
enum BgColor { BG_WHITE = 0xFF, BG_LIGHT_GRAY = 0xCC, BG_DARK_GRAY = 0x88 };

struct Sensor {
    const char* title;
    const char* subtitle;       // Affiché sous le titre quand state_entity est vide (mode CTA)
    const char* state_entity;   // "" → pas de fetch HA, le bouton est purement CTA
    const char* action_service; // "" → pas d'action
    const char* action_entity;
    BgColor     bgColor;        // BG_WHITE | BG_LIGHT_GRAY | BG_DARK_GRAY
};

// ─── Déclaration des boutons ──────────────────────
// Ajoutez / supprimez des entrées librement.
// NUM_SENSORS et la grille s'adaptent automatiquement.
// Si le total dépasse GRID_COLS × GRID_ROWS, les excédents sont ignorés.
// Si le total est inférieur, les cases vides restent blanches.
Sensor sensors[] = {
    // title        subtitle         state_entity                  action_service                  action_entity                        bgColor
    {"Salon",       "",              "sensor.temperature_salon",   "light/toggle",                 "light.salon",                       BG_WHITE},
    {"Bureau",      "",              "sensor.temperature_bureau",  "light/toggle",                 "light.bureau",                      BG_WHITE},
    {"Cuisine",     "",              "sensor.temperature_cuisine", "switch/toggle",                "switch.cafetiere",                  BG_WHITE},
    {"Extérieur",   "",              "sensor.outdoor_temp",        "",                             "",                                  BG_LIGHT_GRAY},
    {"Humidité",    "",              "sensor.humidity",            "",                             "",                                  BG_LIGHT_GRAY},
    {"VMC",         "",              "sensor.vmc_status",          "fan/toggle",                   "fan.vmc",                           BG_WHITE},
    {"Solaire",     "",              "sensor.pv_power",            "",                             "",                                  BG_LIGHT_GRAY},
    {"Batterie",    "",              "sensor.battery_level",       "",                             "",                                  BG_LIGHT_GRAY},
    {"Garage",      "",              "binary_sensor.garage",       "cover/toggle",                 "cover.garage",                      BG_WHITE},
    {"Portail",     "",              "binary_sensor.portail",      "switch/toggle",                "switch.portail",                    BG_WHITE},
    {"Eau",         "",              "sensor.water_meter",         "",                             "",                                  BG_LIGHT_GRAY},
    {"Gaz",         "",              "sensor.gas_meter",           "",                             "",                                  BG_LIGHT_GRAY},
    {"Conso",       "",              "sensor.power_usage",         "",                             "",                                  BG_LIGHT_GRAY},
    {"Mode",        "",              "input_select.house_mode",    "input_select/select_next",     "input_select.house_mode",           BG_WHITE},
    // Exemple de bouton CTA pur (pas de state_entity) :
    {"Alarme",      "Arm Home",      "",                           "alarm_control_panel/alarm_arm_home", "alarm_control_panel.maison", BG_DARK_GRAY},
    {"Refresh",     "Forcer MAJ",    "",                           "homeassistant/update_entity",  "sensor.time",                       BG_LIGHT_GRAY},
};

// Nombre de capteurs déduit automatiquement du tableau
constexpr int NUM_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

// Cache des états (seulement pour les entrées ayant un state_entity)
String statesCache[NUM_SENSORS];

const char* headerTitles[] = {"CLIMAT", "ÉNERGIE", "ACCÈS", "CONTRÔLE"};

// ═══════════════════════════════════════════════════
//  REQUÊTES ASYNCHRONES (FreeRTOS)
// ═══════════════════════════════════════════════════
struct HARequest { String service; String entity; };

void haTask(void *pvParameters) {
    HARequest* req = (HARequest*)pvParameters;
    if (req->service.length() > 0) {
        HTTPClient http;
        String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM
                     + "/api/services/" + req->service;
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
    xTaskCreate(haTask, "HA_Task", 4096, req, 1, NULL);
}

// ═══════════════════════════════════════════════════
//  HELPERS HOME ASSISTANT
// ═══════════════════════════════════════════════════
String fetchSensorState(const char* entity_id) {
    if (strlen(entity_id) == 0) return "";          // CTA pur, pas de fetch
    if (WiFi.status() != WL_CONNECTED) return "WiFi?";
    HTTPClient http;
    String url = String("http://") + HA_HOST_ADDR + ":" + HA_PORT_NUM
                 + "/api/states/" + entity_id;
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
    if (http.GET() == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        http.end();
        String s = doc["state"].as<String>();
        if (s == "on")          return "OUI";
        if (s == "off")         return "NON";
        if (s == "unavailable") return "---";
        return s;
    }
    http.end();
    return "-";
}

// ═══════════════════════════════════════════════════
//  HELPERS FRAMEBUFFER
// ═══════════════════════════════════════════════════

// Retourne la zone pixel d'une cellule de la grille
Rect_t getCellArea(int index) {
    int maxCells = GRID_COLS * GRID_ROWS;
    if (index >= maxCells) return {0, 0, 0, 0};

    int cellW = SCREEN_WIDTH  / GRID_COLS;
    int cellH = (SCREEN_HEIGHT - HEADER_H) / GRID_ROWS;
    int col   = index % GRID_COLS;
    int row   = index / GRID_COLS;
    return (Rect_t){
        .x = col * cellW,
        .y = HEADER_H + row * cellH,
        .width  = cellW,
        .height = cellH
    };
}

// Inverse (noir↔blanc) tous les pixels d'une zone rectangulaire dans le framebuffer.
// Le framebuffer EPD47 est en 4 bpp → 1 octet = 2 pixels.
// ~0xFF = 0x00 (blanc→noir) et ~0x00 = 0xFF (noir→blanc), idem pour les gris.
void invertRegion(Rect_t area) {
    int stride = EPD_WIDTH / 2;                 // octets par ligne
    int xByteStart = area.x / 2;               // alignement sur 2 px
    int xByteEnd   = (area.x + area.width) / 2;
    for (int y = area.y; y < area.y + area.height; y++) {
        for (int x = xByteStart; x < xByteEnd; x++) {
            framebuffer[y * stride + x] = ~framebuffer[y * stride + x];
        }
    }
}

// ═══════════════════════════════════════════════════
//  DESSIN DES ÉLÉMENTS
// ═══════════════════════════════════════════════════
void drawHeaderToBuffer() {
    epd_fill_rect(0, 0, SCREEN_WIDTH, HEADER_H, 0xDD, framebuffer);
    epd_draw_line(0, HEADER_H - 1, SCREEN_WIDTH, HEADER_H - 1, 0x00, framebuffer);

    int sectionW = SCREEN_WIDTH / GRID_COLS;
    for (int i = 0; i < GRID_COLS; i++) {
        if (i > 0)
            epd_draw_line(i * sectionW, 0, i * sectionW, HEADER_H, 0x00, framebuffer);

        const char* label = (i < (int)(sizeof(headerTitles) / sizeof(headerTitles[0])))
                            ? headerTitles[i] : "ZONE";
        int tx = i * sectionW + sectionW / 4;
        int ty = 40;
        writeln((GFXfont *)&FiraSans, label, &tx, &ty, framebuffer);
    }
}

// Dessine une cellule dans le framebuffer.
// "valueOrSubtitle" : état HA ou sous-titre CTA.
// "pressed"         : true → la case sera inversée après le dessin normal,
//                     ce qui garantit que texte ET fond s'inversent correctement.
void drawCellToBuffer(int index, const String& valueOrSubtitle, bool pressed = false) {
    Rect_t area = getCellArea(index);
    if (area.width == 0) return;

    // ── Fond de base (couleur configurée par le champ bgColor) ──────────
    epd_fill_rect(area.x, area.y, area.width, area.height,
                  (uint8_t)sensors[index].bgColor, framebuffer);

    // ── Bordure ──────────────────────────────────────────────────────────
    int cx = area.x + MARGIN / 2;
    int cy = area.y + MARGIN / 2;
    int w  = area.width  - MARGIN;
    int h  = area.height - MARGIN;
    epd_draw_rect(cx, cy, w, h, 0x00, framebuffer);

    // ── Titre ─────────────────────────────────────────────────────────────
    int tx = cx + 10;
    int ty = cy + 35;
    writeln((GFXfont *)&FiraSans, sensors[index].title, &tx, &ty, framebuffer);

    // ── Valeur ou sous-titre ──────────────────────────────────────────────
    if (valueOrSubtitle.length() > 0) {
        tx = cx + 15;
        ty = cy + 90;
        writeln((GFXfont *)&FiraSans, valueOrSubtitle.c_str(), &tx, &ty, framebuffer);
    }

    // ── Inversion si bouton pressé ────────────────────────────────────────
    // On dessine toujours en noir sur fond clair, puis on inverse la zone.
    // Résultat : fond noir, texte blanc → parfaitement lisible sur e-ink.
    if (pressed) {
        invertRegion(area);
    }
}

// Envoie uniquement la zone d'une cellule vers l'écran (partial refresh)
void pushCellToScreen(int index) {
    Rect_t area = getCellArea(index);
    if (area.width == 0) return;
    epd_poweron();
    epd_draw_grayscale_image(area, framebuffer);
    epd_poweroff();
}

// ═══════════════════════════════════════════════════
//  LOGIQUE DE MISE À JOUR
// ═══════════════════════════════════════════════════

// Rafraîchissement complet toutes les 30 minutes (nettoyage physique e-ink)
void performFullRefresh() {
    Serial.println("[TIMER] Full Screen Refresh (30 min)");
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    drawHeaderToBuffer();

    int maxCells = GRID_COLS * GRID_ROWS;
    for (int i = 0; i < maxCells; i++) {
        if (i < NUM_SENSORS) {
            statesCache[i] = fetchSensorState(sensors[i].state_entity);
            // Pour un CTA pur, on affiche le sous-titre à la place de l'état
            String display = (strlen(sensors[i].state_entity) == 0)
                             ? String(sensors[i].subtitle)
                             : statesCache[i];
            drawCellToBuffer(i, display, false);
        }
        // Les cases sans capteur restent blanches (déjà remplies par memset)
    }

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    lastFullRefresh  = millis();
    lastSensorUpdate = millis();
}

// Mise à jour légère toutes les minutes (partial refresh sur les cases modifiées)
void performSensorUpdate() {
    Serial.println("[TIMER] Sensor Data Update (1 min)");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (strlen(sensors[i].state_entity) == 0) continue;  // CTA : on ne poll pas

        String newState = fetchSensorState(sensors[i].state_entity);
        if (newState != statesCache[i]) {
            statesCache[i] = newState;
            drawCellToBuffer(i, newState, false);
            pushCellToScreen(i);
        }
    }
    lastSensorUpdate = millis();
}

// ═══════════════════════════════════════════════════
//  GESTION TACTILE
// ═══════════════════════════════════════════════════
void handleTouch(int16_t tx, int16_t ty) {
    if (ty < HEADER_H) return;  // bandeau en-tête ignoré

    int cellW  = SCREEN_WIDTH  / GRID_COLS;
    int cellH  = (SCREEN_HEIGHT - HEADER_H) / GRID_ROWS;
    int col    = tx / cellW;
    int row    = (ty - HEADER_H) / cellH;
    int index  = row * GRID_COLS + col;

    if (index < 0 || index >= min(NUM_SENSORS, GRID_COLS * GRID_ROWS)) return;

    Serial.printf("[TOUCH] Bouton %d — %s\n", index, sensors[index].title);

    // 1 ── Bouton pressé : fond inversé (noir) + texte inversé (blanc)
    String display = (strlen(sensors[index].state_entity) == 0)
                     ? String(sensors[index].subtitle)
                     : statesCache[index];
    drawCellToBuffer(index, display, /*pressed=*/true);
    pushCellToScreen(index);

    // 2 ── Envoi de la commande HA en arrière-plan (non bloquant)
    sendHAActionAsync(index);

    // 3 ── Attente 1 seconde (feedback visuel)
    delay(1000);

    // 4 ── Récupération du nouvel état et retour à l'affichage normal
    if (strlen(sensors[index].state_entity) > 0) {
        statesCache[index] = fetchSensorState(sensors[index].state_entity);
        display = statesCache[index];
    }
    drawCellToBuffer(index, display, /*pressed=*/false);
    pushCellToScreen(index);
}

// ═══════════════════════════════════════════════════
//  ARDUINO SETUP / LOOP
// ═══════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    epd_init();

    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("[ERROR] Impossible d'allouer le framebuffer !");
        while (true);
    }

    Wire.begin(BOARD_SDA, BOARD_SCL);
    touch.setPins(-1, TOUCH_INT);
    if (touch.begin(Wire, 0x5D, BOARD_SDA, BOARD_SCL)) {
        touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(false, true);
    } else {
        Serial.println("[WARN] Écran tactile non initialisé.");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Connexion");
    while (WiFi.status() != WL_CONNECTED) { Serial.print('.'); delay(500); }
    Serial.println(" OK");

    performFullRefresh();
}

void loop() {
    int16_t tx, ty;
    if (touch.getPoint(&tx, &ty)) {
        handleTouch(tx, ty);
    }

    unsigned long now = millis();
    if (now - lastFullRefresh >= FULL_REFRESH_INTERVAL) {
        performFullRefresh();
    } else if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
        performSensorUpdate();
    }

    delay(20);
}
