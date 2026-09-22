# ESP32 Greenhouse Gateway/Controller (ATOMIC)

ESP32-based gateway/controller firmware for an orchid-greenhouse IoT/WSN system
(team final project, 2025–2026): it aggregates sensor nodes, drives greenhouse
equipment (blower, exhaust fan, dehumidifier) via relay/SSR panels, and syncs
with a cloud backend using store-and-forward caching for unreliable field links.

- 10 ESP8266 sensor nodes + 2 ESP32 gateway/controllers (this repo: the gateway)
- Local caching, connection recovery, threshold/schedule control, LCD/RTC/SD card
- WebSocket diagnostics, WebSerial admin, device configuration portal, OTA
- Wi-Fi with GPRS (SIM800) fallback

Companion public repo: [esp8266-sensor-node](https://github.com/dhimasardinata/esp8266-sensor-node)
(sensor-node firmware). Author portfolio: <https://dhimasardinata.netlify.app/>.

## Hardware

- ESP32 (ESP32dev), SIM800 modem, relay/SSR board, I²C LCD + RTC, SD card
- Pin map and per-site relay assignment in `include/config.h`
  (`GH_ID_CONFIG` selects site 1 or 2)

## Build

PlatformIO, two environments (one per site):

```bash
pio run -e gh1   # site 1
pio run -e gh2   # site 2
```

Dependencies resolve automatically from `platformio.ini`
(ArduinoJson, RTClib, LiquidCrystal_I2C, TinyGSM, NTPClient,
ESPAsyncWebServer, AsyncTCP). Partition map: `partitions_custom.csv`.

## Configuration (do this first)

All secrets ship as placeholders — the firmware will not reach your backend
until you set them, either in code or (recommended) via the on-device web
portal, which persists them to flash:

| Placeholder | Meaning |
|---|---|
| `Greenhouse-1` / `Greenhouse-2` | Site Wi-Fi SSIDs |
| `change-me-wifi-password` | Site Wi-Fi password |
| `change-me-admin-password` | WebSerial/AP admin password |
| `PASTE_API_TOKEN_HERE` / `PASTE_TA_API_TOKEN_HERE` | Backend API tokens |
| `https://your-server.example.com/api` | Telemetry/control API base |
| `https://your-ta-server.example.com/api` | Schedule/device-status API base |
| `https://your-relay-server.example.com/api` | Relay/proxy API base |
| `abcdefghijklmnopqrstuvwxyz123456` | 32-char local AES key (replace with your own) |

## Field behavior (from the 10-node deployment, 25 Feb–31 Mar 2026)

Store-and-forward caching cut telemetry loss from ~51–53% to ~12–19%,
with ~42–44% of uploads delivered via cache.

## License

CC BY-NC-SA 4.0 — see `LICENSE`.
