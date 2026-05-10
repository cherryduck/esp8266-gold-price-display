# ESP8266 Gold/Silver Price Display

A real-time commodity price display built on an ESP8266 D1 Mini, showing live gold (XAU) or silver (XAG) prices in GBP on an 8-digit 7-segment display.

## Features

- **Live prices** fetched every 5 minutes from [gold-api.com](https://www.gold-api.com)
- **Dual asset support** — toggle between Gold (XAU) and Silver (XAG)
- **Persistent settings** — asset selection survives reboots (EEPROM)
- **Easy configuration** — WiFiManager captive portal with asset dropdown
- **No wiring needed for config** — double-reset the board to open the config portal

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | ESP8266 D1 Mini |
| Display | MAX7219 8-digit 7-segment module |
| Power | 5V USB |

### Wiring

| MAX7219 | D1 Mini Pin | GPIO |
|---------|-------------|------|
| DIN     | D7          | GPIO13 |
| CS      | D6          | GPIO12 |
| CLK     | D5          | GPIO14 |
| VCC     | 5V          | — |
| GND     | GND         | — |

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org) (CLI or VS Code extension)
- USB cable connected to D1 Mini

### Build & Flash

1. Clone this repository:
   ```bash
   git clone https://github.com/cherryduck/esp8266-gold-price-display.git
   cd esp8266-gold-price-display
   ```

2. Connect the D1 Mini via USB, then build and flash:
   ```bash
   pio run --target upload
   ```

> **Note:** If your serial port is not `/dev/ttyUSB0`, update `upload_port` and `monitor_port` in `platformio.ini`.

### First Boot

After flashing, the board boots and the display briefly shows `88888888` as a test pattern, then enters WiFi setup mode:

1. Connect to the `GoldPrice-Setup` WiFi access point from your phone or laptop
2. Enter your WiFi credentials on the captive portal page
3. Select your preferred asset (Gold or Silver) from the dropdown
4. Save — the board connects to your network and starts displaying prices

No API key is needed. The firmware uses the free tier of [gold-api.com](https://www.gold-api.com).

### Serial Monitoring

```bash
pio device monitor
```

## Configuration

### Changing Asset (Gold / Silver)

Double-reset the board within 5 seconds to open the config portal:

1. Press the D1 Mini RESET button
2. Press RESET again within 5 seconds
3. Connect to the `GoldPrice-Setup` WiFi AP
4. Change your asset selection in the portal and save

The config portal password is `hermes123`.

### Changing WiFi Network

Same double-reset procedure — the captive portal shows your WiFi credentials fields where you can enter a new network.

### Serial Output

The board logs all activity to serial at 115200 baud, including:
- WiFi connection status
- API fetch results
- Display updates
- EEPROM read/write operations
- Double-reset detection

## Technical Details

| Setting | Value |
|---------|-------|
| API endpoint | `https://api.gold-api.com/price/{XAU\|XAG}/GBP` |
| Refresh interval | 5 minutes |
| HTTP timeout | 10 seconds |
| Display brightness | 8/15 |
| EEPROM usage | 4 bytes (mode + boot tick) |
| Config portal timeout | 3 minutes (auto), 5 minutes (forced) |

### Libraries

| Library | Version |
|---------|---------|
| WiFiManager | ^2.0.17 |
| ArduinoJson | ^7.4 |
| LedControl | ^1.0.6 |

### Error Indicators

| Display | Meaning |
|---------|---------|
| `Err -` | Never received a valid price |
| `---- ----` (blinking) | Temporary fetch failure |

## License

MIT
