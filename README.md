# ESP32 Temperature Logger

A WiFi-enabled temperature and humidity data logger built on the **ESP32-C3** microcontroller. The device periodically reads sensor data and securely uploads it to a Google Cloud Function endpoint, then enters deep sleep to conserve power.

## Features

- 🌡️ Reads temperature and humidity from a **DHT22** sensor
- ☁️ Uploads data via **HTTPS POST** to a Google Cloud Function
- 💤 **Deep sleep** between readings (~10 µA sleep current) for battery-powered deployments
- 🔄 **Retry logic** with exponential backoff — persists retry state across sleep cycles using RTC memory
- 📡 **DNS caching** to reduce connection overhead
- 🔁 **Dual-fallback HTTPS** strategy (direct IP + hostname) for improved reliability
- 🕐 **ISO 8601 timestamps** synchronized via NTP
- 🔑 **Secrets isolation** — credentials stored in a gitignored `config.h` file
- 🛠️ Utility sketches for standalone sensor and WiFi RSSI testing

## Hardware Requirements

| Component | Details |
|-----------|---------|
| Microcontroller | ESP32-C3 DevKitM-1 |
| Sensor | DHT22 (temperature & humidity) |
| Sensor pin | GPIO1 |
| Power | USB or battery |

## Project Structure

```
esp32-temperature-logger/
├── platformio.ini                       # PlatformIO build configuration
├── src/
│   ├── TempLogger_node02_esp32c3.cpp    # Main application
│   ├── config.h.template                # Configuration template (copy → config.h)
│   ├── sensor_serial_test.cpp.txt       # Standalone DHT sensor test utility
│   └── wificheck.cpp.txt                # WiFi RSSI monitor utility
├── include/                             # Custom header files
├── lib/                                 # Custom libraries
└── test/                                # Unit tests (PlatformIO test runner)
```

## Getting Started

### Prerequisites

- [PlatformIO IDE](https://platformio.org/) (VS Code extension recommended) or PlatformIO Core CLI
- A Google Cloud Function that accepts JSON temperature readings (see [Payload Format](#payload-format))

### 1. Clone the repository

```bash
git clone https://github.com/alisoltani82-netizen/esp32-temperature-logger.git
cd esp32-temperature-logger
```

### 2. Create your configuration file

Copy the template and fill in your credentials:

```bash
cp src/config.h.template src/config.h
```

Edit `src/config.h`:

```cpp
#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASS     "your-wifi-password"

// Google Cloud Function endpoint
#define FUNCTION_URL  "https://your-region-your-project.cloudfunctions.net/ingest"
#define FUNCTION_HOST "your-region-your-project.cloudfunctions.net"

// 64-character hex device authentication key
#define DEVICE_KEY    "your-64-character-hex-device-key-here"

// Unique identifier for this device
#define DEVICE_ID     "node-2"
```

> **Note:** `config.h` is listed in `.gitignore` and will never be committed to the repository.

### 3. Build and upload

```bash
# Build
pio run

# Upload to device
pio run -t upload

# Open serial monitor
pio device monitor
```

> Update `monitor_port` in `platformio.ini` to match your system's serial port (e.g., `/dev/ttyUSB0` on Linux, `COM9` on Windows).

## Configuration

Key parameters in `TempLogger_node02_esp32c3.cpp`:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `SAMPLE_MS` | `3600000` ms (1 hour) | Interval between readings |
| `RETRY_INTERVAL_MS` | `30000` ms (30 s) | Interval between retry attempts |
| `RETRY_MAX_DURATION_MS` | `300000` ms (5 min) | Maximum total retry window |
| `DHTPIN` | `1` | GPIO pin connected to DHT22 data line |
| `DHTTYPE` | `DHT22` | Sensor model |
| `SEND_DUMMY` | `false` | Set `true` to send synthetic data without a sensor |

## How It Works

```
Boot / Wake from sleep
        │
        ▼
  Initialize DHT22
  Connect to WiFi
  Sync time via NTP
        │
        ▼
  Read temperature & humidity
  (retry once on NaN)
        │
        ▼
  HTTP POST JSON to Cloud Function ──► Success ──► Sleep 1 hour
        │
        ▼ (on failure)
  Enter retry mode (RTC memory)
  Sleep 30 seconds
        │
        ▼
  Wake → retry POST (up to 10 attempts / 5 minutes)
        │
        ▼ (max retries exceeded)
  Give up, sleep 1 hour
```

### Deep Sleep & Power

The device enters deep sleep after every cycle. RTC memory retains retry state across sleep cycles so in-progress retries survive the power-off period.

| State | Approx. current |
|-------|----------------|
| Active (WiFi TX) | 80–100 mA |
| Deep sleep | ~10 µA |

### HTTPS Dual-Fallback Strategy

1. **Attempt 1 – IP-based**: Resolve the Cloud Function hostname via DNS, then connect using the raw IP address with a `Host` header. Bypasses potential DNS-caching or TLS-SNI issues.
2. **Attempt 2 – Hostname-based**: Standard HTTPS connection using the hostname. Used if the IP-based attempt fails.

### Payload Format

The device sends the following JSON body in each POST request:

```json
{
  "device_id": "node-2",
  "timestamp": "2025-10-01T00:34:12-0500",
  "temp_c": 22.5,
  "hum_pct": 55.0
}
```

## Utility Sketches

Two additional reference sketches are included (as `.txt` files to prevent accidental compilation):

| File | Purpose |
|------|---------|
| `src/sensor_serial_test.cpp.txt` | Reads DHT sensor every 5 seconds and prints to Serial. Useful for verifying wiring before deployment. |
| `src/wificheck.cpp.txt` | Exposes a `/rssi` HTTP endpoint to monitor live WiFi signal strength. Useful for site surveys and debugging connectivity. |

To use either sketch, rename it to `.cpp` and rename the main application file so only one `setup()`/`loop()` pair is compiled.

## Serial Log Prefixes

| Prefix | Meaning |
|--------|---------|
| `[WiFi]` | WiFi connection events |
| `[DNS]` | DNS resolution results |
| `[POST]` | Upload request details |
| `[POST-IP]` | IP-based connection attempt |
| `[POST-HOST]` | Hostname-based connection attempt |
| `[RETRY]` | Retry logic messages |
| `[WARN]` | Warnings |
| `[SLEEP]` | Deep sleep information |
| `[WiFiEvent]` | Raw WiFi event codes |

## PlatformIO Configuration

```ini
[env:esp32c3]
platform  = espressif32
board     = esp32-c3-devkitm-1
framework = arduino

; Native USB (CDC) — required for ESP32-C3
board_build.arduino.usb_mode      = cdc
board_build.arduino.usb_cdc_on_boot = 1
build_flags = -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1

monitor_speed   = 115200
monitor_filters = time, esp32_exception_decoder
```

## License

This project is provided as-is for personal and educational use.
