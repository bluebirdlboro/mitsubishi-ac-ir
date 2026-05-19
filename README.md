# Mitsubishi AC IR Controller (ESP32)

Reverse-engineered infrared protocol for **Mitsubishi Heavy Industries SRK-series** air conditioners. An ESP32 sends and decodes the IR frames and exposes full AC control over MQTT with **Home Assistant MQTT Discovery**. No cloud, no proprietary hub, no vendor app — just an ESP32, an IR LED, and a local broker.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![HA Compatible](https://img.shields.io/badge/Home%20Assistant-MQTT%20Auto%20Discovery-blue)](https://www.home-assistant.io/)

---

## What This Does

| Capability | Detail |
|---|---|
| Modes | Cool / Heat / Dry / Fan Only / Off |
| Fan speeds | Low / Medium / High (protocol has no true Auto — see notes) |
| Temperature | 16–30 °C, 1 °C step |
| MQTT | Separate command topics for `mode`, `temp`, `fan`, `preset`; retained state topic |
| HA Discovery | Auto-registers as a `climate` entity |
| IR Receive | Decodes the original remote's frames and mirrors state back to HA |
| Scene presets | Configurable presets (boost cool / comfort / quiet sleep / away-off) |

---

## Hardware

```
┌──────────┐     IR LED     ┌──────────────┐
│  ESP32   │─────▶▶▶────────│  Mitsubishi  │
│          │    (38 kHz)    │  AC Unit     │
│  GPIO 4  │                │              │
└────┬─────┘                └──────────────┘
     │
     │ WiFi
     ▼
┌──────────┐
│   MQTT   │  (e.g. Mosquitto, HA add-on)
│  Broker  │
└──────────┘
```

**IR transmitter** — IR LED (TSAL6200 or similar) driven by GPIO4 through a 2N2222 NPN in common-emitter:

```
        5V ── 100Ω ── IR LED anode
                       │
                       LED cathode ── 2N2222 collector
                                              │
        GPIO4 ── 1kΩ ── 2N2222 base           │
                                              │
                                  emitter ── GND
```

> The MCU pin drives only the transistor base; LED current comes from the 5 V rail through the limiting resistor. Driving the LED directly from a GPIO pin will under-power it.

**IR receiver (optional)** — VS1838B on **GPIO14** (adjust `IR_RECV_PIN` in `src/aircon/main.cpp` to match your module's silkscreen). Used only for capturing frames from the original remote and reflecting AC state back to Home Assistant.

---

## MQTT Topics

The default topic prefix is `camping_ir/` (this project started as a campervan controller — rename freely in `setup()`).

| Topic | Direction | Description |
|---|---|---|
| `camping_ir/cmd/ac_climate/mode` | → | `cool` / `heat` / `dry` / `fan_only` / `auto` / `off` |
| `camping_ir/cmd/ac_climate/temp` | → | Target temperature (16–30) |
| `camping_ir/cmd/ac_climate/fan`  | → | `low` / `medium` / `high` / `auto` |
| `camping_ir/cmd/ac_climate/preset` | → | `强力制冷` / `舒适模式` / `静音睡眠` / `离家关闭` |
| `camping_ir/cmd/ac_climate/set_all` | → | JSON: `{"mode":..,"temperature":..,"fan_speed":..}` |
| `camping_ir/state/ac_climate` | ← | Current AC state (retained, JSON) |
| `camping_ir/state/scene` | ← | Active preset name (retained) |
| `camping_ir/ir_received` | ← | Decoded fields from the original remote |
| `camping_ir/ir_received/raw` | ← | Raw timing dump for unknown frames |
| `home/<device_id>/availability` | ← | LWT `online` / `offline` |

---

## Build & Deploy

```bash
# 1. Clone
git clone https://github.com/bluebirdlboro/mitsubishi-ac-ir.git
cd mitsubishi-ac-ir

# 2. Set up secrets
cp include/secrets.h.template include/secrets.h
# Edit include/secrets.h — set WIFI_SSID, WIFI_PASSWORD, and OTA_PASSWORD.
# Do not leave OTA_PASSWORD empty in production; an empty password lets
# anyone on your LAN re-flash the device.

# 3. First flash over USB
# In platformio.ini, uncomment the two `esptool` lines and comment the
# two `espota` lines, then:
pio run -e aircon-esp32 -t upload

# 4. Subsequent updates over OTA
# Revert platformio.ini to the espota lines and set upload_port to
# your ESP32's mDNS hostname (matches OTA_HOSTNAME in main.cpp).
pio run -e aircon-esp32 -t upload
```

The MQTT broker host (`MQTT_HOST`) is currently hard-coded near the top of `src/aircon/main.cpp` — change it to point at your broker.

---

## Protocol Notes

Mitsubishi Heavy Industries ships several different IR protocols across its SRK line. The `IRremoteESP8266` library covers the **88-bit** and **152-bit** variants out of the box. The unit this firmware was built against uses a shorter, custom **8-byte** frame that none of the upstream libraries recognised, so it was captured with a logic analyser and replayed via `IRsend::sendRaw`.

**Captured frame layout (8 bytes, LSB-first on the wire):**

```
Byte 0:  0xFF                       Fixed header
Byte 1:  0x00                       Fixed
Byte 2:  Fan speed (high byte)      0xFF=High  0xBF=Medium  0x9F=Low
Byte 3:  Fan speed (low byte)       0x00=High  0x40=Medium  0x60=Low
Byte 4:  Temp (high nibble) | mode  Temp = (32 - target_C) & 0x0F
                                    Mode low nibble:
                                      0x3=Heat 0x4=Fan 0x5=Dry
                                      0x6=Cool 0x7=Auto
                                    Mode bit3 set = power off
Byte 5:  0xFF - Byte 4              One's-complement checksum of Byte 4
Byte 6:  0x2A                       Fixed trailer
Byte 7:  0xD5                       Fixed trailer
```

**Timing (38 kHz carrier):**

| Element | Mark (µs) | Space (µs) |
|---|---|---|
| Header  | 5950 | 7475 |
| Bit `1` | 508  | 3454 |
| Bit `0` | 508  | 1496 |
| Trailer | 508  | 7422 |

The frame is sent three times back-to-back with a 50 ms gap, matching the original remote's behaviour. The receive path in `handleIRReceive()` decodes the same layout from `decode_results::rawbuf` when `IRremoteESP8266` reports `UNKNOWN`.

**Known limitations of this capture:**
- No timer support (the original remote can schedule on/off — not implemented here).
- Swing position is not modulated by this frame — it is on a separate button on the remote that hasn't been captured.
- The Mitsubishi protocol has no genuine "Auto fan" code; selecting `auto` in Home Assistant falls back to `low`.

---

## License

MIT — use it, fork it, build on it. Keep the attribution.

---

## About

Built as part of a home automation setup for an AC model whose protocol wasn't covered by any upstream library. If you need similar work — IR / RF protocol reverse-engineering, ESP32 firmware, or Home Assistant integration — I'm available on [Upwork](https://www.upwork.com/).
