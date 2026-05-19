# 🐧 Mitsubishi AC IR Controller (ESP32)

Reverse-engineered infrared protocol for **Mitsubishi Heavy Industries SRKxx** air conditioners. ESP32 decodes the IR protocol and exposes full AC control via MQTT, with automatic **Home Assistant MQTT Discovery**. No cloud. No proprietary hub. Just an ESP32 with an IR LED.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![HA Compatible](https://img.shields.io/badge/Home%20Assistant-MQTT%20Auto%20Discovery-blue)](https://www.home-assistant.io/)

---

## 🎯 What This Does

| Capability | Detail |
|---|---|
| Modes | Cool / Heat / Dry / Fan Only / Off |
| Fan speeds | Low / Medium / High / Auto |
| Temperature | 16–30°C |
| Swing | Vertical / Horizontal |
| MQTT | Command + State topics, retained state |
| HA Discovery | Auto-registers as climate entity |
| IR Receive | Decodes original remote signals for verification |

---

## 🏗 Hardware

```
┌──────────┐     IR LED     ┌─────────────┐
│  ESP32   │─────▶▶▶────────│  Mitsubishi  │
│          │    (38 kHz)    │  AC Unit     │
│  GPIO 4  │                │              │
└────┬─────┘                └─────────────┘
     │
     │ WiFi
     ▼
┌──────────┐
│   MQTT   │
│  Broker  │  (e.g. Mosquitto, HA add-on)
└──────────┘
```

**Wiring** — IR LED (TSAL6200 or similar) on GPIO4 through a 2N2222 transistor:

```
ESP32 GPIO4 ──┬── 100Ω ── IR LED Anode ── IR LED Cathode ── GND
              └── 1kΩ ── 2N2222 Base
                          Collector ── IR LED Cathode
                          Emitter ── GND
```

> An IR receiver (VS1838B) on GPIO21 is optional — used only for protocol verification.

---

## 📦 MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `camping_ir/cmd/ac_climate` | → | Set mode/temp/fan via JSON |
| `camping_ir/cmd/ac_climate/mode` | → | Set mode directly |
| `camping_ir/cmd/ac_climate/temp` | → | Set temperature |
| `camping_ir/cmd/ac_climate/fan` | → | Set fan speed |
| `camping_ir/cmd/ac_climate/preset` | → | Set scene preset |
| `camping_ir/state/ac_climate` | ← | Current AC state (retained) |
| `camping_ir/ir_received` | ← | Raw decoded IR from remote |

---

## 🔧 Build & Deploy

```bash
# 1. Clone
git clone https://github.com/bluebirdlboro/mitsubishi-ac-ir.git
cd mitsubishi-ac-ir

# 2. Set up secrets
cp include/secrets.h.template include/secrets.h
# Edit include/secrets.h with your WiFi credentials

# 3. Build & flash (USB first time)
pio run -e aircon-esp32 -t upload

# 4. After first flash, switch to OTA
# In platformio.ini, comment esptool lines, uncomment espota lines
# Change upload_port to your ESP32's mDNS name
```

---

## 🔬 Protocol Discovery

The Mitsubishi Heavy Industries protocol uses **17-byte frames** at 38 kHz with the following structure:

```
Byte 0:   Always 0x52 (header)
Byte 1:   0x71 (fixed)
Byte 2:   0x00 or 0x01 (swing control)
Byte 3:   Mode byte (see table)
Byte 4:   Temperature (0x10–0x1E → 16–30°C, subtract 0x10)
Byte 5:   Fan speed
Byte 6-7: Timer (0x0000 = off)
Byte 8-9: Unknown / checksum candidates
Byte 10-16: Fixed trailer pattern
```

Captured and verified with a **logic analyzer (Saleae clone)** on the original remote's IR output, then replicated with `IRremoteESP8266`.

---

## 📝 License

MIT — use it, fork it, build on it. Just keep the attribution.

---

## 👋 About This Project

Built as part of a home automation setup. All IR protocol work was done from scratch — no existing library had complete support for this specific AC model line.

If you need custom IR protocol work (reverse-engineering, ESP32 firmware, Home Assistant integration), I'm available on Upwork.

---

*Blog post coming soon: "Reverse-Engineering Mitsubishi AC IR Protocol on ESP32"*
