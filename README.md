# ESP8266 Gold/Silver Price Display

A real-time commodity price display built on an ESP8266 D1 Mini, showing live gold (XAU) or silver (XAG) prices in GBP on an 8-digit 7-segment display.

## Features

- **Live prices** fetched every 5 minutes from [gold-api.com](https://www.gold-api.com)
- **Dual asset support** — toggle between Gold (XAU) and Silver (XAG)
- **Persistent settings** — asset selection survives reboots (EEPROM)
- **Easy configuration** — WiFiManager captive portal with asset dropdown
- **Automatic setup** — config portal opens on first boot and when the network is unreachable

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

To reconfigure the asset or WiFi settings, the config portal opens automatically if the board cannot connect to your saved network. To force it open:

1. Disable or rename your WiFi network so the board cannot connect
2. Wait for the board to detect the connection failure (config portal opens automatically)
3. Connect to the `GoldPrice-Setup` WiFi AP from your phone or laptop
4. Change your settings in the portal and save

Alternatively, after 5 consecutive API fetch failures (~25 minutes), the board will automatically open the config portal for reconfiguration.

### Changing WiFi Network

The config portal opens automatically when the saved network is unavailable. Simply connect to the `GoldPrice-Setup` WiFi AP and enter your new network credentials.

### Serial Output

The board logs all activity to serial at 115200 baud, including:
- WiFi connection status
- API fetch results
- Display updates
- EEPROM read/write operations

## Technical Details

| Setting | Value |
|---------|-------|
| API endpoint | `https://api.gold-api.com/price/{XAU\|XAG}/GBP` |
| Refresh interval | 5 minutes |
| HTTP timeout | 10 seconds |
| Display brightness | 8/15 |
| EEPROM usage | 1 byte (mode only) |
| Config portal timeout | 3 minutes |
| Auto-portal trigger | 5 consecutive fetch failures |

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
