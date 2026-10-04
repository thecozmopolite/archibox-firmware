# ArchiBox USB Service — Windows

> Service Windows qui démarre automatiquement quand la clé USB est insérée, et se désinstalle quand elle est retirée.

---

## 🎯 Concept

```
Clé USB ArchiBox
├── archibox-agent.exe      (agent compilé)
├── archibox-agent.py       (source Python)
├── config.json             (config device ID + server)
├── install.bat             (installe le service)
├── uninstall.bat            (désinstalle le service)
└── autorun.inf             (démarre agent.exe au branchement)
```

Quand la clé est insérée → le service `archibox-agent` démarre.
Quand la clé est retirée → le service s'arrête et se désinstalle automatiquement.

---

## ⚙️ Détails d'implémentation

### USB Detect — Comment ça marche

**Option A : `autorun.inf` (simple mais désactivé sur Windows 10+)**
```ini
[autorun]
open=archibox-agent.exe
icon=archibox-agent.exe
label=ArchiBox
```

**Option B : Windows Service avec WMI USB monitoring (fiable)**
- Le service tourne en permanence
- Écoute les événements WMI `Win32_VolumeChangeEvent`
- Détecte l'insertion du volume `D:\` (lettre assignée à la clé)
- Au démarrage : vérifie si la clé est présente → lance l'agent
- À l'arrêt : cleanly shutdown l'agent

### Service Python

```python
# archibox-agent.py — fonctionne sur le PC Windows
# Communique avec ESP32 via WiFi (même protocole que archimade)
# Et avec archimade:8766 via HTTP
```

**Responsabilités :**
1. Lire les commandes depuis archimade:8766 (`GET /poll/{deviceId}`)
2. Transformer les commandes en paquets série/USB vers l'ESP32
3. Recevoir les données ESP32 et les forwarder à archimade
4. Logger l'activité
5. Auto-détection de la déconnexion USB → shutdown propre

**Communication ESP32 ↔ Agent Windows :**
- L'agent ouvre le port série USB de l'ESP32 (émulé sur le PC)
- L'ESP32 envoie ses données (capteurs, boutons, etc.) via ce port
- L'agent forward tout à archimade:8766

---

## 📁 Structure des fichiers

```
USB_KEY/
├── archibox-agent/          # code source
│   ├── agent.py             # agent principal
│   ├── serial_bridge.py      # bridge USB ↔ WiFi
│   ├── config.py            # configuration
│   ├── service_wrapper.py   # wrapper Windows service
│   ├── install_service.py   # script d'installation
│   └── requirements.txt     # dépendances Python
│
├── build/                   # sortie pyinstaller
│   └── archibox-agent.exe
│
├── config.json              # config device ID
├── install.bat              # double-click pour installer
└── uninstall.bat            # double-click pour désinstaller
```

---

## 🔧 Installation / Désinstallation

### Sur la clé USB — `install.bat`
```bat
@echo off
powershell -Command "Start-Process python -ArgumentList 'install_service.py' -Verb RunAs"
```

### Sur le PC cible — `install_service.py` (run as admin)
```python
import win32serviceutil, win32service, win32event
import subprocess, sys, os

# Installe le service "ArchiBoxAgent"
subprocess.run([
    sys.executable, "service_wrapper.py", "--startup", "auto", "install"
])
```

### Désinstallation automatique (sur retrait USB)
```python
# Dans le service : quand le volume USB disparaît
win32serviceutil.RemoveService("ArchiBoxAgent")
```

---

## 🔄 Protocole Agent Windows ↔ Archimade

```
┌─────────────────┐       WiFi        ┌──────────────────┐
│  ESP32-S3-Box   │◄─── USB serial ───►│ Windows Agent    │
│  (archibox)     │                   │ (archibox-agent) │
└────────┬────────┘                   └────────┬─────────┘
         │                                     │
         │ serial                              │ HTTP/TCP
         │                              ┌─────▼──────┐
         │                              │ archimade  │
         │                              │  :8766     │
         │                              └────────────┘
```

L'agent Windows joue le rôle que archimade:8766 joue maintenant, mais en local sur le PC.

---

## 📋 Tâches restantes

- [ ] Tester `pyinstaller` pour compiler `archibox-agent.exe`
- [ ] Implémenter le bridge série USB → WiFi dans l'agent
- [ ] Tester l'auto-détection USB sur Windows
- [ ] Générer les scripts `install.bat` / `uninstall.bat`
