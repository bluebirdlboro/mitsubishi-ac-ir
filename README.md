# Mitsubishi AC IR Controller (ESP32)

Reverse-engineered infrared protocol for Mitsubishi Heavy Industries SRK-series air conditioners. An ESP32 transmits the captured 8-byte frame on a 38 kHz carrier and exposes full AC control via MQTT, with automatic Home Assistant MQTT Discovery. No cloud, no proprietary hub.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![HA Compatible](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-blue)](https://www.home-assistant.io/)

---

## Features

| Capability | Detail |
|---|---|
| Modes | Cool / Heat / Dry / Fan Only / Off |
| Fan speeds | Low / Medium / High |
| Temperature | 16-30 C, 1 C steps |
| MQTT | Command + retained state topics |
| HA Discovery | Auto-registers as a climate entity |
| IR Receive | Decodes the original remote for state sync |
| Scenes | Power Cool, Comfort, Quiet Sleep, Away/Off presets |

---

## Hardware

```
+----------+     IR LED      +--------------+
|  ESP32   |---->>>----------|  Mitsubishi  |
|          |    (38 kHz)     |  AC Unit     |
|  GPIO 4  |                 |              |
+----+-----+                 +--------------+
     |
     | WiFi
     v
+----------+
|   MQTT   |
|  Broker  |  (e.g. Mosquitto, HA add-on)
+----------+
```

Wiring - IR LED driven via a 2N2222 transistor so the GPIO does not source the full LED current:

```
ESP32 GPIO4 --+-- 1k --- 2N2222 Base
              |          Collector --- IR LED Cathode
              |          Emitter   --- GND
              |
              +-- (no direct LED path; LED sits between 3V3 -> 100R -> Anode,
                   Cathode goes to the transistor collector above)
```

An IR receiver (e.g. VS1838B) on GPIO14 is optional and used only for decoding the original remote so the device's MQTT state stays in sync when someone presses the physical remote.

---

## Protocol

The captured frame is **8 bytes**, sent three times back-to-back with a 50 ms gap.

| Byte | Meaning |
|---|---|
| B0 | `0xFF` header |
| B1 | `0x00` header |
| B2 | Fan speed primary (`0xFF` high, `0xBF` medium, `0x9F` low) |
| B3 | Fan speed complement (`0x00` / `0x40` / `0x60`) |
| B4 | High nibble: `(32 - temp) & 0xF`; low nibble: mode (`6` cool, `5` dry, `4` fan, `3` heat). Bit 3 of the low nibble set = power off. |
| B5 | `0xFF - B4` checksum |
| B6 | `0x2A` trailer |
| B7 | `0xD5` trailer |

Timing (microseconds):

| Symbol | Mark | Space |
|---|---|---|
| Header | 5950 | 7475 |
| Bit 1 | 508 | 3454 |
| Bit 0 | 508 | 1496 |
| Trailer | 508 | 7422 |

Bits are emitted LSB-first within each byte. Captured with a logic analyser on the original remote's IR LED line and replayed via `IRremoteESP8266`'s `sendRaw`.

---

## MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `camping_ir/cmd/ac_climate/mode` | -> device | `off`, `cool`, `heat`, `dry`, `fan_only`, `auto` |
| `camping_ir/cmd/ac_climate/temp` | -> device | Integer 16-30 |
| `camping_ir/cmd/ac_climate/fan` | -> device | `low`, `medium`, `high` |
| `camping_ir/cmd/ac_climate/preset` | -> device | `Power Cool`, `Comfort`, `Quiet Sleep`, `Away/Off` |
| `camping_ir/cmd/ac_climate/set_all` | -> device | JSON `{mode, temperature, fan_speed}` for one-shot setting |
| `camping_ir/state/ac_climate` | <- device | JSON `{mode, temperature, fan_mode, preset_mode}` (retained) |
| `camping_ir/state/scene` | <- device | Current scene name (retained) |
| `camping_ir/ir_received` | <- device | Decoded frame when the original remote is used |
| `home/<device_id>/availability` | <- device | `online` / `offline` (LWT) |

---

## Build and Deploy

```bash
# 1. Clone
git clone https://github.com/bluebirdlboro/mitsubishi-ac-ir.git
cd mitsubishi-ac-ir

# 2. Set up secrets
cp include/secrets.h.template include/secrets.h
# Edit include/secrets.h with WiFi, OTA, and MQTT credentials.

# 3. Build and flash over USB the first time
#    In platformio.ini, uncomment the esptool/upload_port lines and
#    comment out the espota lines.
pio run -e aircon-esp32 -t upload

# 4. After the first flash, switch back to OTA in platformio.ini and set
#    upload_port to the device's mDNS name.
```

---

## Known Limitations

- **No auto fan speed.** The captured protocol only encodes three fixed fan speeds (low / medium / high). HA's `auto` fan mode is intentionally not exposed.
- **No swing control.** The remote's swing buttons send a separate frame that has not been reverse-engineered yet.
- **Open-loop send.** The device transmits and assumes the AC received the frame - there is no acknowledgement from the unit. The IR receiver provides indirect confirmation when someone uses the physical remote.
- **Single unit per device.** No remote address is encoded in the frame, so every AC in IR range will respond.

---

## License

MIT - use it, fork it, build on it.
