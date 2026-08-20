# AQM — Air Quality Monitor: Technical Report

**Author:** Partha A B  
**Version:** 1.0  
**Platform:** ESP32 + Firebase + Android (Capacitor)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Hardware Design](#3-hardware-design)
4. [ESP32 Firmware — Deep Dive](#4-esp32-firmware--deep-dive)
   - 4.1 [Dual-Core Architecture](#41-dual-core-architecture)
   - 4.2 [Non-Blocking Sensor State Machines](#42-non-blocking-sensor-state-machines)
   - 4.3 [Multi-WiFi Priority System](#43-multi-wifi-priority-system)
   - 4.4 [OLED Display](#44-oled-display)
   - 4.5 [Buzzer Logic](#45-buzzer-logic)
   - 4.6 [Firebase Sync Task (Core 0)](#46-firebase-sync-task-core-0)
5. [Firebase Database Design](#5-firebase-database-design)
6. [Android App — Deep Dive](#6-android-app--deep-dive)
   - 6.1 [Technology Stack](#61-technology-stack)
   - 6.2 [Dashboard](#62-dashboard)
   - 6.3 [Analytics & Charts](#63-analytics--charts)
   - 6.4 [Settings Page](#64-settings-page)
   - 6.5 [Back Button Navigation](#65-back-button-navigation)
7. [Air Quality Index Calculations](#7-air-quality-index-calculations)
8. [Data Flow — End to End](#8-data-flow--end-to-end)
9. [Known Design Decisions & Trade-offs](#9-known-design-decisions--trade-offs)

---

## 1. Project Overview

The **AQM (Air Quality Monitor)** is a full-stack IoT project that monitors indoor air quality in real time. It is composed of three tightly integrated layers:

1. **Hardware layer** — An ESP32 microcontroller connected to a CO₂ sensor, a laser particle counter, an environmental sensor, an OLED display, a buzzer, and a push button.
2. **Cloud layer** — Firebase Realtime Database (RTDB), which acts as a live synchronisation hub between the hardware and the app.
3. **Application layer** — An Android app (built as a Progressive Web App wrapped in Capacitor) that displays live readings, historical charts, and provides remote control of the device.

What makes this project technically interesting is not just the sensors — it is the **real-time, bidirectional communication** between embedded hardware running FreeRTOS and a mobile app communicating via a cloud REST API. The device can be monitored and controlled without any physical access, including updating its WiFi credentials remotely.

---

## 2. System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                          ESP32 DevKit V1                         │
│                                                                  │
│   ┌─────────────────────────┐    ┌───────────────────────────┐  │
│   │      CORE 1 (App CPU)   │    │   CORE 0 (Protocol CPU)   │  │
│   │                         │    │                           │  │
│   │  • MTP80-A state machine│    │  • Firebase HTTP uploads  │  │
│   │  • BME680 async read    │    │  • Buzzer config poll     │  │
│   │  • PMS7003 byte parser  │    │  • WiFi config poll       │  │
│   │  • AQI / IAQ calculator │    │  • Auto-reconnect         │  │
│   │  • OLED 1s refresh      │    │  • NTP time sync          │  │
│   │  • Button debouncer     │    │                           │  │
│   │  • Buzzer oscillator    │    │  FreeRTOS Task (16KB)     │  │
│   │                         │──▶│  Signalled by Core 1 flag │  │
│   └─────────────────────────┘    └───────────┬───────────────┘  │
│                                              │                  │
└──────────────────────────────────────────────┼──────────────────┘
                                               │ HTTPS REST
                                               ▼
                                   ┌───────────────────────┐
                                   │   Firebase RTDB       │
                                   │  (asia-southeast1)    │
                                   │                       │
                                   │  /readings/current    │
                                   │  /history             │
                                   │  /config/wifi         │
                                   │  /config/buzzerEnabled│
                                   └───────────┬───────────┘
                                               │ HTTPS REST
                                               ▼
                                   ┌───────────────────────┐
                                   │   Android App         │
                                   │  (Capacitor PWA)      │
                                   │                       │
                                   │  Dashboard            │
                                   │  Analytics / Charts   │
                                   │  Settings             │
                                   │  Remote Buzzer Toggle │
                                   └───────────────────────┘
```

The critical insight is that Firebase acts as a **message bus** — neither the ESP32 nor the app connect to each other directly. The ESP32 writes readings, the app reads them. The app writes config, the ESP32 reads it. This decoupling makes the system robust: if the ESP32 restarts, the app still shows the last known values; if the app is closed, the ESP32 keeps running.

---

## 3. Hardware Design

### Pinout Table

| GPIO | Connection | Protocol |
|------|-----------|----------|
| GPIO16 (RX2) | MTP80-A TX | UART @ 9600 baud |
| GPIO17 (TX2) | MTP80-A RX | UART @ 9600 baud |
| GPIO25 (RX1) | PMS7003 TX | UART @ 9600 baud |
| GPIO26 (TX1) | PMS7003 RX | UART @ 9600 baud |
| GPIO21 (SDA) | SSD1306 + BME680 | I²C @ 400 kHz |
| GPIO22 (SCL) | SSD1306 + BME680 | I²C @ 400 kHz |
| GPIO4 | Push Button → GND | INPUT_PULLUP |
| GPIO18 | Active Buzzer | Digital OUT |

### Sensor Specifications

**MTP80-A (NDIR CO₂ Sensor)**  
Measures CO₂ concentration using Non-Dispersive Infrared absorption — an IR light source shines through the air sample; CO₂ molecules absorb specific IR wavelengths. The sensor communicates over UART with a 14-byte binary frame. A warmup period applies after power-on; the firmware checks a validity flag byte (`mtpBuf[11]`) to suppress readings during warmup.

**PMS7003 (Laser Particle Counter)**  
Uses a 650nm laser and a photodetector. As air is drawn through by an internal fan, particles scatter the laser light. The scattering pattern and intensity allows categorisation into PM1.0, PM2.5, and PM10 size bins (µg/m³). It outputs a continuous 32-byte binary frame at 9600 baud.

**BME680 (Environmental Sensor)**  
Combines a barometric pressure sensor, capacitive humidity sensor, temperature sensor, and a Metal Oxide Semiconductor (MOS) gas sensor on a single chip. The MOS element works by heating a metal oxide substrate to ~320°C — gases in the air adsorb onto the surface and change the electrical resistance. Higher resistance = cleaner air. This is used to calculate a VOC estimate and an IAQ score. The sensor requires a warmup period (typically 20–30 seconds after power-on) for the heater to stabilise — this is why the buzzer is suppressed for the first 20 seconds.

**SSD1306 OLED (128×64)**  
The OLED uses the Adafruit SSD1306 library over I²C. The firmware draws a 15-pixel yellow header bar (achieved by filling a white rectangle — the physical OLED has yellow pixels in the top rows) and uses the remaining 49 pixels for blue data (the lower region of the physical OLED has blue pixels). This is a hardware characteristic of the specific display panel, not software colouring.

---

## 4. ESP32 Firmware — Deep Dive

### 4.1 Dual-Core Architecture

The ESP32's two Xtensa LX6 cores run at 240 MHz each. The Arduino `setup()` and `loop()` functions run on **Core 1** by default. The firmware deliberately splits its workload:

| Core 1 (Main Loop) | Core 0 (FreeRTOS Task) |
|---|---|
| Poll button (~1ms latency) | Firebase HTTP PUT (readings) |
| Drive buzzer (500ms toggle) | Firebase HTTP POST (history) |
| Refresh OLED (every 1s) | Firebase HTTP GET (buzzer config) |
| Read MTP80-A (state machine) | Firebase HTTP GET (WiFi config) |
| Read PMS7003 (byte parser) | Multi-WiFi reconnection |
| Read BME680 (async) | NTP time sync |
| Calculate AQI/IAQ | — |

**Why this separation matters:**  
An HTTP request to Firebase can take anywhere from 80ms to 800ms depending on network latency. If this happened on Core 1, the button would feel unresponsive and the OLED could skip update cycles. By pinning all HTTP to Core 0 via a FreeRTOS task, **Core 1 never blocks** — the button always responds in under 2ms, the OLED updates exactly every second.

**Inter-core communication** is done through a `volatile` flag and a snapshot struct:

```cpp
volatile bool   firebaseSyncReady = false;  // Written by Core 1, read by Core 0
FirebaseSnapshot fbSnapshot;                 // Written by Core 1 before flag is set

// Core 1 (in loop()):
fbSnapshot = { data.co2, data.co2Valid, data.pm1_0, ... };
firebaseSyncReady = true;  // Signal Core 0

// Core 0 (in firebaseSyncTask()):
if (firebaseSyncReady) {
  FirebaseSnapshot s = fbSnapshot;  // Take a local copy immediately
  firebaseSyncReady = false;        // Release as fast as possible
  // Now use 's' for HTTP — Core 1 can freely update fbSnapshot again
}
```

The `volatile` keyword prevents compiler optimisation from caching the flag in a register, ensuring Core 0 always reads the latest value from RAM.

### 4.2 Non-Blocking Sensor State Machines

A naive implementation would do `Serial.readBytes()` for 14 bytes with a timeout — this blocks the loop for up to 700ms. The AQM firmware instead uses **state machines** that collect bytes over multiple loop iterations.

**MTP80-A State Machine:**
```
MTP_IDLE ──(timer fires, send command)──▶ MTP_READING
                                               │
                               (bytes arrive one per loop iteration)
                                               │
                               (14 bytes received)──▶ MTP_IDLE (parse & validate)
                               (700ms timeout) ──▶ MTP_IDLE (discard)
```

The loop runs at ~50,000 iterations per second. Each iteration calls `mtpUpdate()`, which checks if any bytes are in the UART buffer and appends them to `mtpBuf[]`. When the 14th byte arrives, the full frame is validated by checksum and parsed. Zero blocking.

**BME680 Async Reading:**  
The BME680 library provides `beginReading()` which starts the measurement and returns the time (in ms) when the result will be ready. The firmware stores this time and calls `endReading()` only when `millis()` has passed that value. No `delay()` required.

### 4.3 Multi-WiFi Priority System

The firmware stores up to 3 WiFi networks in **NVS (Non-Volatile Storage)** flash using the `Preferences` library — credentials survive power cuts without re-flashing.

**Boot-time connection (in `setup()`):**  
The connection sequence is: Slot 1 → Slot 2 → Slot 3 → **Hardcoded Fallback (Appu)**.

Each slot gets 20 retries × 500ms = **10 seconds**. If all three configured slots fail (e.g., wrong credentials were pushed from the app), the firmware unconditionally tries the hardcoded home network. If the fallback connects, it **self-heals**: it resets both NVS and the in-memory credentials back to the default, so the next reboot goes straight to Slot 1 correctly.

```
Boot WiFi sequence:
  Slot 1 (NVS) → fail → Slot 2 (NVS) → fail → Slot 3 (NVS) → fail
                                         ↓
                              Hardcoded fallback: Appu
                                         ↓ (if connects)
                              Reset NVS to Appu + update in-memory
                              → Device is back online, Firebase can be fixed from app
```

The hardcoded SSID/password lines are marked with `// ← CHANGE HERE (Ctrl+F 'CHANGE HERE')` in the firmware for easy location in Arduino IDE when the permanent home network changes.

**Runtime credential update (in `firebaseSyncTask()`):**  
Every 3 seconds, Core 0 fetches `/config/wifi.json` from Firebase. If any credential has changed:
1. Updates in-memory variables
2. Saves to NVS via `preferences.putString()`
3. Sets `needsReconnect = true`

Core 0 picks this up and calls `connectMultiWiFi()`. A **30-second cooldown** prevents reconnect storms — if all slots fail, the task waits 30s before trying again (instead of hammering `WiFi.disconnect()` in a tight loop, which would prevent the radio from ever associating).

**App-side safety guard:**  
The app reads `/status/wifi.json` (published by the ESP32 every sync) to know which SSID is currently active. When the user presses "Apply Changes" in the WiFi settings, the app checks that at least one of the 3 new slots contains the currently active SSID. If it doesn't, the save is blocked with an error: *"Cannot remove 'Appu' — it's the WiFi the device is currently using."* This is the software guard; the hardcoded fallback is the firmware-level guard.

**Practical use:**  
At home → Slot 1 is `Appu`. Going to college → open app → set Slot 1 to `CollegeWiFi`, keep Slot 2 as `Appu`. Apply. ESP32 picks up in 3s, saves, reconnects to CollegeWiFi. When you're back home and CollegeWiFi isn't reachable, it falls through to Slot 2 (`Appu`) automatically.


### 4.4 OLED Display

The OLED shows 5 pages, cycled by the push button. Each page uses the same layout: a white-filled 15px header bar (which appears yellow on this OLED panel) with the page title and mute indicator, followed by white text on the dark blue body.

| Page | Contents |
|------|---------|
| 1/5 AIR QUALITY | AQI (large), CO₂ (ppm), IAQ score |
| 2/5 CO₂ & IAQ | CO₂ large display, VOC ppm |
| 3/5 PARTICLES | PM1.0, PM2.5, PM10 µg/m³ |
| 4/5 CLIMATE | Temperature °C, Humidity %, Pressure hPa |
| 5/5 SYSTEM | WiFi status + slot number, Buzzer state |

When the buzzer is muted, `[M]` appears in the top-right of the yellow header bar on every page.

### 4.5 Buzzer Logic

The active buzzer oscillates at 2Hz (500ms ON / 500ms OFF) when dangerous conditions are detected. Conditions that trigger the buzzer:
- CO₂ > 1000 ppm
- PM2.5 > 35 µg/m³
- AQI > 100
- IAQ > 150

Two suppression layers exist:
1. **Warmup suppression** — `millis() < 20000`. The BME680's MOS heater takes ~20s to stabilise. Before this, gas resistance is artificially high (appearing as very bad air), which would cause false alarms. The 20s window prevents this.
2. **App mute** — `buzzerEnabled` flag, readable/writable via Firebase. The app sends `true`/`false` to `/config/buzzerEnabled.json`; the ESP32 reads this every 3 seconds.

### 4.6 Firebase Sync Task (Core 0)

The task runs in an infinite FreeRTOS loop with a 200ms sleep between iterations. Its logic each cycle:

```
1. Check: is WiFi connected or needsReconnect set?
      YES → connectMultiWiFi() and continue to next cycle
      
2. Check: has 3 seconds passed since last config poll?
      YES → GET /config/buzzerEnabled.json
           → GET /config/wifi.json → compare → save if changed → set needsReconnect if changed
           
3. Check: has Core 1 set firebaseSyncReady?
      YES → copy snapshot → clear flag → PUT /readings/current.json → POST /history.json
```

This design means config changes (buzzer, WiFi) feel near-real-time (~3s latency), while sensor data uploads happen on the 10-second cadence set by Core 1.

---

## 5. Firebase Database Design

```
(root)
├── readings/
│   └── current/           ← Overwritten every 10s by ESP32
│       ├── co2            int    ppm
│       ├── aqi            int    0–500
│       ├── iaq            int    0–500
│       ├── pm1_0          int    µg/m³
│       ├── pm2_5          int    µg/m³
│       ├── pm10           int    µg/m³
│       ├── temp           float  °C
│       ├── hum            float  %
│       ├── pres           float  hPa
│       ├── gas            float  kΩ
│       ├── voc_ppm        float  ppm
│       └── timestamp      int    Unix ms (NTP) or millis()
│
├── history/               ← Appended every 10s (Firebase auto-key)
│   ├── -NxAbc123.../
│   └── -NxDef456.../
│
└── config/
    ├── buzzerEnabled      bool   true/false
    └── wifi/
        ├── ssid1          string
        ├── pass1          string
        ├── ssid2          string
        ├── pass2          string
        ├── ssid3          string
        ├── pass3          string
        └── updatedAt      int    Unix ms
```

**Timestamp strategy:** The ESP32 syncs with NTP on boot (IST = UTC+5:30). If NTP sync succeeds, timestamps are Unix milliseconds. If not (no internet at boot), `millis()` is used as a fallback — these are relative to boot time and are clearly identifiable in the database.

---

## 6. Android App — Deep Dive

### 6.1 Technology Stack

| Layer | Technology |
|---|---|
| UI | HTML5 + Vanilla CSS (Tailwind CDN) |
| Logic | Vanilla JavaScript (ES2020) |
| Charts | Chart.js |
| Icons | Lucide Icons |
| Native wrapper | Capacitor v7 |
| Backend | Firebase RTDB REST API (fetch) |
| Android target | API 26+ (Android 8.0+) |

The app is a **Progressive Web App (PWA)** wrapped in Capacitor. This means the UI is entirely web-based (HTML/CSS/JS) but it runs as a native Android app with access to native APIs (back button, status bar color, etc.). This approach allowed rapid development while still producing a real `.apk`.

### 6.2 Dashboard

The dashboard polls Firebase's `/readings/current.json` every 10 seconds and updates the UI. Each sensor card shows:
- Live value with unit
- A colour-coded status badge (Good / Moderate / Unhealthy / Hazardous)
- A small sparkline showing the last few readings

The **online/offline status** is determined by checking if the `timestamp` in Firebase is recent (within the last ~20 seconds relative to the current time). If the ESP32 has stopped uploading (powered off, no WiFi), the app shows "Offline".

The **Buzzer toggle** is event-driven: pressing the switch immediately sends a `PUT` request to Firebase with the new boolean value. The ESP32 picks it up within 3 seconds.

### 6.3 Analytics & Charts

The Analytics tab fetches the last N entries from `/history` and plots them using Chart.js. Each sensor has its own expandable chart that opens in a full-screen modal with zoom support.

### 6.4 Settings Page

The Settings panel slides in from the right (CSS `translateX` animation). It contains two sub-pages navigated via a menu list:

**Update WiFi:** Three SSID + password pairs (Slot 1 = primary, Slot 2 = backup, Slot 3 = fallback). Passwords can be revealed with the eye icon toggle. Pressing "Apply Changes" sends a `PUT` to `/config/wifi.json`. The ESP32 detects this change within 3 seconds and reconnects.

**About:** Technical information about sensors, architecture, and credits.

### 6.5 Back Button Navigation

The Capacitor `@capacitor/app` plugin intercepts the Android hardware back button. The navigation hierarchy is:

```
Chart modal open?  → Close modal
Settings sub-view open? → Back to settings menu
Settings open? → Close settings
Analytics tab? → Go to Dashboard
Dashboard? → Exit app
```

---

## 7. Air Quality Index Calculations

### AQI (US EPA PM2.5 Scale)

The US EPA AQI is calculated by piecewise linear interpolation of PM2.5 concentration:

| PM2.5 (µg/m³) | AQI Range | Category |
|---|---|---|
| 0 – 12 | 0 – 50 | Good |
| 12.1 – 35.4 | 51 – 100 | Moderate |
| 35.5 – 55.4 | 101 – 150 | Unhealthy for Sensitive Groups |
| 55.5 – 150.4 | 151 – 200 | Unhealthy |
| 150.5 – 250.4 | 201 – 300 | Very Unhealthy |
| 250.5 – 500 | 301 – 500 | Hazardous |

### IAQ (Indoor Air Quality Index)

IAQ is a composite score calculated from BME680 gas resistance and humidity:

```
humScore = |humidity - 40.0| × 1.25      (40% RH is ideal)
gasScore  = (gasKΩ < 50) ? (50 - gasKΩ) × 4.0 : 0.0
IAQ       = clamp(humScore + gasScore + 25, 0, 500)
```

Higher gas resistance (cleaner air) produces a lower IAQ score. The +25 baseline accounts for normal indoor baseline pollution.

### VOC Estimate

```
voc_ppm = clamp((100.0 / gasKΩ) × 0.35,  0.05, 25.0)
```

This is an approximation — accurate VOC measurement requires a calibration baseline that varies by environment and the BME680's specific gas reference resistance. This is a reasonable relative indicator.

---

## 8. Data Flow — End to End

**Sensor reading → App display:**
```
BME680 heater fires (Core 1)
       ↓ 150ms later
endReading() returns data (Core 1)
       ↓
AQI/IAQ calculated (Core 1)
       ↓ every 10s
fbSnapshot struct populated (Core 1)
firebaseSyncReady = true (Core 1)
       ↓ within 200ms
Core 0 wakes, copies snapshot
       ↓ ~100-500ms HTTP
PUT /readings/current.json (Core 0)
       ↓
App polls Firebase every 10s
       ↓
Chart.js UI updates
```

**App button press → ESP32 response:**
```
User taps "Mute Buzzer" in app
       ↓ immediate
PUT /config/buzzerEnabled = false (Fetch API)
       ↓
Firebase RTDB updated
       ↓ within 3s
Core 0 GET /config/buzzerEnabled.json
buzzerEnabled = false (Core 0)
       ↓ volatile read
Core 1 buzzer logic: no alert even if isDangerous
```

---

## 9. Known Design Decisions & Trade-offs

| Decision | Rationale | Trade-off |
|---|---|---|
| Firebase free tier REST API | No SDK dependency, simple HTTP | Max ~50K reads/day; config polling every 3s uses ~28K reads/day |
| No RTOS mutex on fbSnapshot | Core 1 writes before setting flag; Core 0 copies immediately | Theoretically possible tearing if struct write is non-atomic on 64-bit members; acceptable for this use case |
| WiFi credentials in Firebase (not encrypted) | Allows OTA config update from app | Open Firebase rules mean URL holders can see credentials |
| `vTaskDelay` in Core 0, `delay` in setup | FreeRTOS scheduler is not running during setup | Two code paths for WiFi connection (boot vs runtime) |
| 10s sensor upload interval | Keeps Firebase within free tier limits | Data freshness is 10s old in the app |
| 20s buzzer warmup | BME680 MOS sensor requires thermal stabilisation | First 20s after boot, even real hazardous conditions won't trigger the buzzer |
| Hardcoded fallback WiFi in firmware | Device can never be permanently locked out by bad Firebase/NVS data | Changing the permanent home WiFi requires re-flashing the firmware (or using Ctrl+F 'CHANGE HERE') |
| App blocks removing active SSID | Prevents the chicken-and-egg disconnection problem | User must keep active SSID in at least one slot; cannot fully clear all slots while connected |
| 30s reconnect cooldown | Prevents WiFi radio reset storm when all slots fail | Up to 30s delay before retrying after a complete failure |

---

*Report generated for AQM v1.0 — Designed & Built by Partha A B*
