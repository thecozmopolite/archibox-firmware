# ArchiBox ESP32-S3 Firmware

Firmware pour Seeed XIAO ESP32-S3 avec microphone I2S (INMP441).

## Fonctionnalités

- **Micro I2S** — INMP441, 16kHz mono 16-bit
- **WiFi** — Connexion automatique à `ChezWam`, credentials stockés en EEPROM
- **OTA** — Mise à jour sans fil depuis archimade (`/ota/latest`)
- **Bouton physique** — 1 clic = validation archimade, 2 clics = mute/unmute, appui long = mute forcé
- **LED** — éteinte = mic muted, allumée = mic actif, clignote = activité réseau
- **Serial** — Protocole texte pour agent PC (MUTE, UNMUTE, PING, STATUS, WIFI:, OTA_CHECK)

## Commandes série (Agent → ESP)

```
MUTE        — coupe le micro
UNMUTE      — rallume le micro
STATUS      — affiche état (muted, wifi, IP, version)
PING        — pong
WIFI:<ssid>:<password> — configure WiFi et stocke en EEPROM
OTA_CHECK   — vérifie et installe une mise à jour si disponible
```

## Protocole audio

L'agent PC demande un chunk audio à l'ESP, qui répond :

```
SIZE:NNNNN\n
<NNNNN octets de PCM 16-bit mono 16kHz>
###END###\n
```

## Installation

```bash
pio run -e seeed_xiao_esp32s3 --target upload
```

## Architecture

- ESP32 : acquisition audio + HID USB + contrôle LED + bouton
- Agent PC : transcription Whisper + client archimade + HID forwarding
- archimade : STT/LLM/TTS + routage par TOKEN
