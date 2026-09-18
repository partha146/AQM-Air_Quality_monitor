# 🌬️ AQM — Air Quality Monitor

> A real-time indoor air quality monitoring system that integrates an **ESP32 microcontroller** with multiple environmental sensors, uploads live data to **Firebase Realtime Database**, and visualises everything in a cross-platform **Android app** built with Capacitor.

---

## ✨ Features

- 📡 **Real-time sensor data** — CO₂, PM1.0, PM2.5, PM10, Temperature, Humidity, Pressure, VOC, AQI, IAQ
- 🔥 **Firebase RTDB integration** — live readings synced every 10 seconds, full history log
- 📱 **Android App** (Capacitor) — live dashboard, historical charts, buzzer mute toggle, WiFi management
- 🖥️ **SSD1306 OLED** — 5-page local display with yellow header bar and blue data body; shows active WiFi slot number
- 🔊 **Active buzzer alerts** — triggers on hazardous CO₂, PM2.5, AQI or IAQ levels; 20s warmup suppression on boot
- 📶 **Multi-WiFi with hardcoded fallback** — up to 3 OTA-updatable networks (via app → Firebase); always falls back to a hardcoded home WiFi so the device can never be permanently locked out by bad credentials
- ⚡ **Dual-core non-blocking architecture** — sensors & display on Core 1, all HTTP on Core 0

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                        ESP32                            │
│                                                         │
│  Core 1 (App CPU)          Core 0 (Protocol CPU)        │
│  ┌─────────────────┐       ┌──────────────────────────┐ │
│  │ Sensor polling  │──────▶│  Firebase HTTP uploads   │ │
│  │ OLED refresh    │ flag  │  Buzzer/WiFi config poll │ │
│  │ Button handler  │       │  Multi-WiFi reconnect    │ │
│  │ Buzzer control  │       │  NTP time sync           │ │
│  └─────────────────┘       └──────────────────────────┘ │
└────────────────────┬────────────────────────────────────┘
                     │  WiFi / Firebase
                     ▼
          ┌─────────────────────┐
          │  Firebase RTDB      │
          │  /readings/current  │
          │  /history           │
          │  /config/wifi       │
          │  /config/buzzer     │
          └──────────┬──────────┘
                     │  REST API
                     ▼
          ┌─────────────────────┐
          │   Android App       │
          │   (Capacitor PWA)   │
          │   Dashboard · Charts│
          │   Settings · About  │
          └─────────────────────┘
```

---

## 🔩 Hardware

| Component | Description |
|---|---|
| **ESP32 DevKit V1** | Dual-core 240 MHz microcontroller with WiFi |
| **MTP80-A** | NDIR CO₂ sensor (0–5000 ppm) via UART |
| **PMS7003** | Laser particle counter — PM1.0, PM2.5, PM10 |
| **BME680** | Temperature, Humidity, Pressure, VOC (I²C) |
| **SSD1306 OLED** | 128×64 monochrome display (I²C) |
| **Active Buzzer** | GPIO-driven alert output |
| **Push Button** | Page-cycle on the OLED |

---

## 📂 Repository Structure

```
AQM/
├── Main_esp32_codes/
│   └── Main_esp32_codes.ino   # ESP32 firmware (Arduino)
├── Phone_app/
│   ├── www/                   # Web app source (HTML/CSS/JS)
│   │   ├── index.html
│   │   ├── app.js
│   │   └── styles.css
│   └── android/               # Capacitor Android project
└── README.md
```

---

## 🚀 Getting Started

### ESP32 Firmware
1. Open `Main_esp32_codes/Main_esp32_codes.ino` in Arduino IDE.
2. Install libraries: `Adafruit SSD1306`, `Adafruit BME680`, `Adafruit GFX`.
3. Set your Firebase URL in `FIREBASE_HOST`.
4. Upload to ESP32 DevKit V1.

### Android App
```bash
cd Phone_app
npm install
npx cap sync android
# Then open android/ in Android Studio and build APK
```

> The app can also be served locally as a web app:
> ```bash
> python -m http.server 8080 -d www
> ```

---

## 📊 Firebase Data Schema

```json
{
  "readings": {
    "current": { "co2": 612, "aqi": 42, "iaq": 38, "pm2_5": 8, "temp": 29.4, "hum": 61.2 }
  },
  "history": { "-NxAbc...": { "co2": 600, "timestamp": 1723000000000 } },
  "config": {
    "buzzerEnabled": true,
    "wifi": { "ssid1": "HomeWiFi", "pass1": "...", "ssid2": "Hotspot", "pass2": "..." }
  }
}
```

---

## 👤 Author

**Partha A B** — Designed, built, and programmed the hardware and software end-to-end.
