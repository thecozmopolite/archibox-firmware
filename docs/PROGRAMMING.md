# ESP32-S3-Box — Guide de Programmation

> Matériel : ESP32-S3-WROOM-1U-N16R8 (16MB Flash, 8MB PSRAM, WiFi+BT 5.0)
> Firmware : ArchiBox (Arduino framework via PlatformIO)

---

## 🖥️ Configuration PlatformIO

```ini
[env:esp32s3]
platform = espressif32
board = esp32s3box
framework = arduino
monitor_speed = 115200
upload_speed = 921600
upload_protocol = esptool          ; USB flash
; upload_protocol = espota          ; OTA (WiFi, après premier flash)
; upload_port = /dev/ttyACM0       ; USB
; upload_port = 192.168.0.194      ; OTA IP
```

### Commandes

```bash
# Build
cd ~/archibox-firmware
pio run -e esp32s3

# Flash USB (premier flash)
pio run -e esp32s3 --target upload

# OTA (après premier flash)
pio run -e esp32s3 --target upload --upload-protocol espota

# Moniteur série
pio device monitor -e esp32s3
```

---

## 🔌 Broches (pins_arduino.h — ESP32-S3-Box)

```
GPIO43 = TX (USB Serial)
GPIO44 = RX (USB Serial)

I2C0 (bus interne) :
  GPIO8  = SDA (ES7210 MIC, ES8311 DAC, ICM42607P IMU, TT21100 Touch)
  GPIO18 = SCL

SPI0 (affichage LCD) :
  GPIO4  = TFT_DC
  GPIO5  = TFT_CS
  GPIO6  = TFT_MOSI
  GPIO7  = TFT_CLK
  GPIO0  = TFT_MISO
  GPIO45 = TFT_BL  (backlight = LED verte)
  GPIO48 = TFT_RST

I2S0 (audio) :
  GPIO47 = I2S_LRCK
  GPIO2  = I2S_MCLK
  GPIO17 = I2S_SCLK
  GPIO16 = I2S_SDIN
  GPIO15 = I2S_DOUT

GPIO46 = PA_PIN     (Ampli audio power)
GPIO1  = MUTE_PIN   (Bouton mute)
GPIO3  = TS_IRQ     (Touch screen IRQ)

ADC :
  GPIO9  = A8 / T9
  GPIO10 = A9 / T10
  GPIO11 = A10 / T11
  GPIO12 = A11 / T12
  GPIO13 = A12 / T13
  GPIO14 = A13 / T14

SPI1 (SD card) :
  GPIO10 = SS
  GPIO11 = MOSI
  GPIO13 = MISO
  GPIO12 = SCK
```

### LED intégrée — CE QUI FONCTIONNE
- **GPIO45 = TFT_BL** →背光 (backlight LCD). Allumée = LED verte allumée au repos.
  - `digitalWrite(GPIO45, LOW)` → LED ON (le backlight LCD tire le courant à travers un transistor)
  - `digitalWrite(GPIO45, HIGH)` → LED OFF
  - ✅ **CONFIRMÉ** : fonctionne au reset (LED verte s'allume)

### LED WS2812B (néopixel)
- Le connecteur JST en position CN1 donne accès à GPIO38
- ⚠️ Non testé encore

---

## 📡 WiFi

```cpp
#define WIFI_SSID     "ChezWam"
#define WIFI_PASS     "RDNSUBXPZIYBFUGU"
```

Le firmware scan les réseaux disponibles, vérifie que `ChezWam` existe, puis se connecte. RSSI moyen : -45 à -65 dBm.

---

## 🔄 Protocole Polling (actuel)

L'ESP32 poll le serveur archimade toutes les 2.5s :

```
ESP32 ──GET /poll/esp32s3-box-01──► archimade:8766
                                          │
                     ┌────────────────────┘
                     │  200 OK: {"cmd": "LED 3"} ou {"cmd": null}
                     ▼
                  ESP32 traite la commande
```

### Commandes supportées
| Commande | Action |
|----------|--------|
| `LED N`  | Fait clignoter la LED N fois |
| `ECHO x` | Affiche x dans le moniteur série |
| `STATUS` | Retourne état WiFi + mémoire |

---

## 🆙 OTA (Over-The-Air)

### Problème connu
`ArduinoOTA.begin()` ne démarre pas le serveur sur le port 3232. Probable cause : le WiFi ou le bootloader de l'ESP32-S3-Box a des contraintes spécifiques.

### Solution : OTA HTTP custom
Le polling WiFi existant est utilisé pour détecter les mises à jour :

```
ESP32 ──GET /poll/esp32s3-box-01──► archimade:8766
                                          │
                        ┌─────────────────▼──────────────────┐
                        │  200 OK: {"cmd": "OTA http://..."} │
                        └──────────────────────────────────┘
                                          │
                     ┌────────────────────┘
                     │  L'ESP32 télécharge le .bin
                     ▼
                  Mise à jour via Update.begin()
```

---

## 🧠 Architecture mémoire

```
Flash totale : 16 MB (13107200 octets)

Partition table default :
  NVS          : 0x00009000 - 0x00040000  (196 KB)
  OTA_DATA     : 0x00040000 - 0x00050000  (64 KB)
  FAT          : 0x00050000 - 0x00290000  (2.25 MB)
  APP0         : 0x00100000 - 0x00600000  (5 MB)
  APP1         : 0x00600000 - 0x00B00000  (5 MB)
  SPIFFS       : 0x00B00000 - 0x00D00000  (2 MB)
  COREDUMP     : 0x00D00000 - 0x00E00000  (1 MB)
  DEFAULT      : 0x00E00000 - 0x01000000  (2 MB)

Firmware flashé en APP0 (0x10000 = 64 KB offset)
OTA partition = APP1
```

---

## 🛠️ Débogage

```bash
# Voir les logs du serveur
tail -f ~/archibox-server/activity.log

# Envoyer une commande manuellement
curl -X POST http://localhost:8766/cmd/esp32s3-box-01 \
  -H "Content-Type: application/json" \
  -d '{"cmd": "LED 3"}'

# Statut serveur
curl http://localhost:8766/status
```

### Codes couleur LED (GPIO45 = TFT_BL)
- Vert clignotant 3x au boot → OK
- Orange permanent → WiFi non connecté
- Rouge 3x → WiFi non trouvé (boucle infinie)
- 2x blanc clignotant → commande reçue

---

## 📦 Dépendances libraries

| Library | Version | Usage |
|---------|---------|-------|
| WiFi | 2.0.0 | Connexion réseau |
| Adafruit NeoPixel | 1.15.5 | LED WS2812B |
| ESPmDNS | (built-in) | mDNS discovery |
| ArduinoOTA | (built-in) | OTA (inactif pour l'instant) |
