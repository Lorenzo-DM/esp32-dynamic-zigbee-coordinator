# Zigbee HA Valve Regulator Coordinator (ESP32-C6)

This project implements a **Zigbee Gateway / Coordinator** centered around an ESP32-C6. It is designed to automate the temperature scheduling of multiple **Zigbee TRVs** (Thermostatic Radiator Valves), such as Sonoff or Moes valves.

## Key Features

- **Automated Scheduling**: Downloads a `config.json` file from a remote server (e.g., Gitea/GitHub) over WiFi to define heating schedules for multiple devices.
- **Dynamic Slot Matching**: Supports multiple time slots per day. Each slot defines a high-temperature period; outside these slots, valves are set to a fallback low temperature.
- **Zigbee <-> WiFi Time Sync**: Connects to WiFi initially to synchronize system time via **SNTP** and fetch the latest configuration.
- **Radio Sharing Optimization**: Automatically shuts down WiFi after synchronization to minimize interference and free up the 2.4GHz radio for the Zigbee stack.
- **Manual Control**: The physical **BOOT button** on the ESP32-C6 acts as a global override, allowing you to toggle all connected valves between High and Low temperatures manually.
- **Auto-Pairing & Identification**: Automatically matches joining Zigbee devices to the configuration using their **IEEE address**.

## Hardware Required

- **ESP32-C6** Development Board.
- One or more **Zigbee TRVs** (e.g., Sonoff TRVZB).
- WiFi environment with internet access for SNTP and JSON fetching.

## Software Configuration

### 1. WiFi & Server Credentials
Modify `main/secrets.h` (using `main/secrets.example` as a template) to set your WiFi SSID, Password, and the Base URL for your configuration server.

### 2. JSON Configuration Format
The project expects a JSON file with the following structure:
```json
{
  "devices": [
    {
      "name": "Living Room",
      "ieee": "00124b002a123456",
      "enabled": true,
      "config": {
        "temp_high": 2100,
        "temp_low": 1700,
        "schedule": [
          { "start": "07:00", "end": "09:00" },
          { "start": "18:00", "end": "22:00" }
        ]
      }
    }
  ]
}
```
*Note: Temperatures are in 1/100ths of a degree (e.g., 2100 = 21.00°C).*

## Build and Flash

1. Set the target to ESP32-C6:
   ```bash
   idf.py set-target esp32c6
   ```
2. Build and flash the firmware:
   ```bash
   idf.py build flash monitor
   ```

## Initialization Sequence

1. **NVS Init**: Prepares internal storage.
2. **WiFi Connect**: Connects to the configured SSID.
3. **HTTP Download**: Fetches the TRV configuration JSON.
4. **SNTP Sync**: Synchronizes the internal RTC with network time.
5. **WiFi Shutdown**: De-initializes WiFi components to optimize Zigbee performance.
6. **Zigbee Start**: Initializes the Zigbee Coordinator and opens the network for pairing.

## Usage

- **Pairing**: Put your TRV into pairing mode. Once it joins the network, the ESP32-C6 will match its IEEE address against the JSON config. If found, it will immediately apply the current scheduled temperature.
- **Monitoring**: Use the serial monitor to view current time, connected devices, and temperature updates.
- **Manual Override**: Press the **BOOT button** to force all valves to "HIGH" temperature mode. Press again to force them to "LOW" mode. The scheduled automation will resume at the next time slot transition.
