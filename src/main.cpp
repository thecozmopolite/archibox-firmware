// ArchiBox Firmware v7 — Seeed XIAO ESP32-S3 + WiFi OTA + I2S + Bouton
// Board: seeed_xiao_esp32s3
//
// WiFi: Obligatoire. SSID/MDP configurables via serial.
// OTA : Check /ota/latest sur archimade au démarrage puis toutes les 5min
//
// Serial protocol (115200 bauds):
//   ESP → Agent:  TOKEN:***\n
//   ESP → Agent:  CMD:<command>\n
//   Agent → ESP:  OK\n / ERR\n / PONG\n / RECORD\n
//   Agent → ESP:  WIFI:<ssid>:<password>\n  → stocke en EEPROM
//   Agent → ESP:  OTA_CHECK\n / OTA_NOW\n
//
// Bouton:
//   1 clic  = signal archimade (validation choix)
//   2 clics = toggle mute/unmute
//   appui long (>800ms) = mute forcé
//
// LED:
//   Led allumée = mic actif (non muted)
//   Led clignote = en cours de connexion WiFi / OTA
//   Led éteinte  = mic muted


#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <EEPROM.h>

// ── Version ────────────────────────────────────────────────────────────────────
#define FIRMWARE_VERSION  7

// ── I2S Mic ─────────────────────────────────────────────────────────────────
#define I2S_WS_PIN   8    // D8 (WS / L/R)
#define I2S_SCK_PIN  9    // D9 (SCK / CLOCK)
#define I2S_SD_PIN   10   // D10 (SD / DOUT)
#define I2S_PORT     I2S_NUM_0
#define SAMPLE_RATE  16000

// ── Bouton ───────────────────────────────────────────────────────────────────
#define BTN_PIN  2   // GPIO2 = D2 sur XIAO, actif bas

// ── LED ──────────────────────────────────────────────────────────────────────
#define LED_PIN  3    // LED jaune intégrée du XIAO (actif HIGH)

// ── Device & Server ──────────────────────────────────────────────────────────
#define DEVICE_ID       "xiao-esp32s3-01"
#define ARCHIMADE_HOST  "192.168.0.119"
#define ARCHIMADE_PORT  8766
#define TOKEN           "archibox-xiao-01"

// ── EEPROM layout ─────────────────────────────────────────────────────────────
#define EEPROM_SIZE     256
#define EEPROM_WIFI_SSID  16   // offset SSID dans EEPROM
#define EEPROM_WIFI_PASS  80   // offset password (64 bytes max)
#define EEPROM_WIFI_OK     0   // byte 0 = 1 si WiFi configuré

// ── Mute state ───────────────────────────────────────────────────────────────
static bool mic_muted = false;
static bool btn_was_pressed = false;

// ── Double-click detection ───────────────────────────────────────────────────
static unsigned long last_click_ms = 0;
static const unsigned long DBLCLICK_MS = 500;    // 2 clics < 500ms = double-clic
static const unsigned long LONGPRESS_MS = 800;   // appui long = mute forcé
static bool longpress_started = false;
static unsigned long longpress_start_ms = 0;

// ── I2S Setup ─────────────────────────────────────────────────────────────────
void i2s_setup() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, .dma_buf_len = 64,
        .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_SCK_PIN, .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD_PIN
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ── LED blink ─────────────────────────────────────────────────────────────────
void led_blink(int times, int ms_on=50, int ms_off=50) {
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(ms_on);
        digitalWrite(LED_PIN, HIGH);
        delay(ms_off);
    }
}

// ── LED status: allumé = mic actif, éteint = muted ──────────────────────────
void led_set(bool on) {
    digitalWrite(LED_PIN, on ? LOW : HIGH);
}

// ── Button events ─────────────────────────────────────────────────────────────
// Retourne: 0=rien, 1=simple clic, 2=double clic, 3=long press
int button_event() {
    static bool pending_simple = false;
    static unsigned long pending_simple_ms = 0;

    bool now = (digitalRead(BTN_PIN) == LOW);
    unsigned long now_ms = millis();

    if (now && !btn_was_pressed) {
        btn_was_pressed = true;
        last_click_ms = now_ms;
        longpress_started = true;
        longpress_start_ms = now_ms;
    }

    if (!now && btn_was_pressed) {
        btn_was_pressed = false;

        if (longpress_started && (now_ms - longpress_start_ms) >= LONGPRESS_MS) {
            longpress_started = false;
            return 3;  // long press
        }

        if ((now_ms - last_click_ms) < DBLCLICK_MS) {
            longpress_started = false;
            pending_simple = false;
            return 2;  // double clic
        }

        longpress_started = false;
        pending_simple = true;
        pending_simple_ms = now_ms;
    }

    if (pending_simple && (now_ms - pending_simple_ms) > DBLCLICK_MS) {
        pending_simple = false;
        return 1;  // simple clic confirmé
    }

    return 0;
}

// ── EEPROM helpers ─────────────────────────────────────────────────────────────
void eeprom_write_wifi(const char* ssid, const char* pass) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EEPROM_WIFI_OK, 1);
    EEPROM.put(EEPROM_WIFI_SSID, ssid);
    EEPROM.put(EEPROM_WIFI_PASS, pass);
    EEPROM.commit();
}

bool eeprom_read_wifi(char* ssid, char* pass) {
    EEPROM.begin(EEPROM_SIZE);
    if (EEPROM.read(EEPROM_WIFI_OK) != 1) return false;
    EEPROM.get(EEPROM_WIFI_SSID, ssid);
    EEPROM.get(EEPROM_WIFI_PASS, pass);
    return strlen(ssid) > 0;
}

// ── WiFi connection ───────────────────────────────────────────────────────────
bool wifi_connect(char* ssid_out, char* pass_out) {
    char ssid[64] = {0}, pass[64] = {0};

    // Essai EEPROM d'abord
    if (eeprom_read_wifi(ssid, pass)) {
        Serial.printf("WIFI: Using EEPROM config for '%s'\n", ssid);
    } else {
        // Défaut: ChezWam
        strncpy(ssid, "ChezWam", 63);
        strncpy(pass, "RDNSUBXPZIYBFUGU", 63);
        Serial.printf("WIFI: Using default config for '%s'\n", ssid);
    }

    if (ssid_out) strncpy(ssid_out, ssid, 64);
    if (pass_out) strncpy(pass_out, pass, 64);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    // Clignote pendant connexion
    led_blink(2, 200, 200);
    WiFi.begin(ssid, pass);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print('.');
        led_blink(1, 100, 400);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWIFI OK  IP=%s  RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        led_blink(4, 50, 50);
        return true;
    } else {
        Serial.println("\nWIFI FAILED");
        return false;
    }
}

// ── OTA setup ─────────────────────────────────────────────────────────────────
void ota_setup() {
    ArduinoOTA.setHostname(DEVICE_ID);
    ArduinoOTA.setPassword("archibox-ota");

    ArduinoOTA.onStart([]() {
        Serial.println("OTA START");
        led_blink(10, 80, 80);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA END  Rebooting...");
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
        if (p % 4096 == 0) Serial.printf("OTA %u%%\n", (p * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("OTA ERROR %d\n", e);
    });

    ArduinoOTA.begin();
    Serial.println("OTA READY");
}

// ── OTA check via HTTP ────────────────────────────────────────────────────────
// Retourne true si mise à jour appliquée et reboot
bool ota_check_and_install() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s:%d/ota/latest?device=%s&v=%d",
        ARCHIMADE_HOST, ARCHIMADE_PORT, DEVICE_ID, FIRMWARE_VERSION);

    Serial.printf("OTA CHECK: %s\n", url);
    http.begin(url);
    int code = http.GET();

    if (code != 200) {
        http.end();
        if (code > 0) Serial.printf("OTA CHECK: nothing new (HTTP %d)\n", code);
        return false;
    }

    String payload = http.getString();
    http.end();

    // Parse JSON: {"version":8,"url":"http://...","md5":"..."}
    int v = 0;
    char firmware_url[256] = {0}, md5sum[64] = {0};

    // Simple JSON parse (pas de bibliothèque JSON)
    auto get_int = [&](const char* key) -> int {
        char* p = strstr((char*)payload.c_str(), key);
        if (!p) return 0;
        while (*p && (*p < '0' || *p > '9')) p++;
        return atoi(p);
    };
    auto get_str = [&](const char* key, char* out, size_t maxlen) {
        char* p = strstr((char*)payload.c_str(), key);
        if (!p) return;
        while (*p && *p != '"') p++;
        if (!*p) return;
        p++; char* q = out;
        while (*p && *p != '"' && (size_t)(q - out) < maxlen - 1) *q++ = *p++;
        *q = 0;
    };

    v = get_int("\"version\"");
    get_str("\"url\"", firmware_url, sizeof(firmware_url));
    get_str("\"md5\"", md5sum, sizeof(md5sum));

    if (v <= FIRMWARE_VERSION) {
        Serial.printf("OTA: v%d available, we have v%d — skip\n", v, FIRMWARE_VERSION);
        return false;
    }

    Serial.printf("OTA: New firmware v%d detected!\n", v);
    led_blink(5, 100, 100);

    // Download and flash
    HTTPClient http_fw;
    http_fw.begin(firmware_url);
    int fcode = http_fw.GET();
    if (fcode != 200) {
        Serial.printf("OTA: Download failed HTTP %d\n", fcode);
        http_fw.end();
        return false;
    }

    int fw_size = http_fw.getSize();
    Serial.printf("OTA: Downloading %d bytes...\n", fw_size);

    // Validate content length fits in OTA partition
    if (!Update.begin(max(fw_size, (int)UPDATE_SIZE_UNKNOWN))) {
        Serial.printf("OTA: Update begin failed\n");
        http_fw.end();
        return false;
    }

    WiFiClient* stream = http_fw.getStreamPtr();
    uint32_t downloaded = 0;
    while (http_fw.connected() && downloaded < fw_size) {
        size_t avail = stream->available();
        if (avail > 0) {
            uint8_t buf[4096];
            size_t got = stream->readBytes(buf, min((size_t)avail, sizeof(buf)));
            Update.write(buf, got);
            downloaded += got;
            if (downloaded % 16384 == 0) {
                Serial.printf("OTA: %u/%u bytes\n", downloaded, (unsigned)fw_size);
            }
        }
        delay(1);
    }
    http_fw.end();

    if (Update.end(true)) {
        Serial.printf("OTA: Install complete (%u bytes). Rebooting...\n", downloaded);
        delay(500);
        ESP.restart();
        return true;  // never reached
    } else {
        Serial.printf("OTA: Update error: %s\n", Update.errorString());
        return false;
    }
}

// ── Handle serial commands from Agent PC ──────────────────────────────────────
void handle_serial_cmd(const String& cmd) {
    if (cmd == "MUTE") {
        mic_muted = true;
        led_set(false);
        Serial.println("MUTED");
    } else if (cmd == "UNMUTE") {
        mic_muted = false;
        led_set(true);
        Serial.println("UNMUTED");
    } else if (cmd == "STATUS") {
        Serial.printf("STATUS: muted=%d wifi=%s ip=%s v=%d\n",
            mic_muted ? 1 : 0,
            WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
            WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "---",
            FIRMWARE_VERSION);
    } else if (cmd == "PING") {
        Serial.println("PONG");
    } else if (cmd.startsWith("WIFI:")) {
        // WIFI:<ssid>:<password>
        String rest = cmd.substring(5);
        int colon = rest.indexOf(':');
        if (colon > 0) {
            String new_ssid = rest.substring(0, colon);
            String new_pass = rest.substring(colon + 1);
            new_ssid.trim();
            new_pass.trim();
            eeprom_write_wifi(new_ssid.c_str(), new_pass.c_str());
            Serial.printf("WIFI: Config saved for '%s'\n", new_ssid.c_str());
            Serial.println("REBOOT");  // signal to reboot
        } else {
            Serial.println("ERR: WIFI:<ssid>:<password>");
        }
    } else if (cmd == "OTA_CHECK") {
        if (ota_check_and_install()) {
            Serial.println("OTA DONE");  // never reached (reboot)
        } else {
            Serial.println("OTA UP2DATE");
        }
    } else if (cmd == "OTA_NOW") {
        // Force reflash current firmware from archimade
        Serial.println("OTA_FORCE");
        // Reboot into bootloader for next OTA
    } else if (cmd == "VERSION") {
        Serial.printf("VERSION:%d\n", FIRMWARE_VERSION);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    pinMode(LED_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP);
    mic_muted = true;  // Mic mute par defaut au demarrage
    led_set(false);    // LED off = muted

    Serial.begin(115200);
    delay(500);

    // Token pour identification agent
    Serial.printf("TOKEN:%s\n", TOKEN);
    Serial.printf("VERSION:%d\n", FIRMWARE_VERSION);

    // I2S mic
    i2s_setup();
    Serial.println("I2S OK");

    // WiFi obligatoire
    if (wifi_connect(nullptr, nullptr)) {
        ota_setup();
        // Check OTA au démarrage
        delay(1000);
        ota_check_and_install();
    } else {
        // WiFi failed — continue sans OTA, LED reste éteinte
        Serial.println("WIFI SKIP — running without network");
    }

    Serial.println("READY");
    // Mic mute par defaut: LED reste eteinte, agent doit envoyer UNMUTE
    led_blink(3, 100, 100);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // ── OTA ──
    ArduinoOTA.handle();

    // Check OTA toutes les 5 minutes
    static unsigned long last_ota_check = 0;
    if (WiFi.status() == WL_CONNECTED && millis() - last_ota_check > 300000) {
        last_ota_check = millis();
        ota_check_and_install();
    }

    // ── WiFi reconnect if down ──
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long last_reconnect = 0;
        if (millis() - last_reconnect > 10000) {
            last_reconnect = millis();
            Serial.println("WIFI: Reconnecting...");
            WiFi.reconnect();
        }
    }

    // ── Serial commands from Agent PC ──
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toUpperCase();
        if (cmd.length() > 0) {
            handle_serial_cmd(cmd);
        }
    }

    // ── Bouton ──
    int ev = button_event();
    if (ev == 1) {
        // Simple clic → archimade validation
        led_blink(1, 200, 50);
        Serial.println("CMD:BUTTON_PRESS");
    } else if (ev == 2) {
        // Double clic → toggle mute
        mic_muted = !mic_muted;
        led_set(!mic_muted);
        Serial.println(mic_muted ? "MUTED" : "UNMUTED");
        led_blink(mic_muted ? 4 : 2, 100, 100);
    } else if (ev == 3) {
        // Long press → mute forcé
        mic_muted = true;
        led_set(false);
        Serial.println("MUTED");
        led_blink(6, 80, 80);
    }

    delay(10);
}
