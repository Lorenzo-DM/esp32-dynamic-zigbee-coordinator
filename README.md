# Zigbee Valve Regulator Coordinator (ESP32-C6)

This project implements a **Zigbee Gateway / Coordinator** centered around an ESP32-C6. It is designed to automate the temperature scheduling of multiple **Zigbee TRVs** (Thermostatic Radiator Valves), such as the Sonoff TRVZB.

## Key Features

- **Automated Scheduling**: Downloads a `config.json` file from a remote server (a Gitea repository by default, see [Configuration URL](#configuration-url)) over WiFi to define heating schedules for multiple devices.
- **Dynamic Slot Matching**: Supports multiple time slots per day. Each slot defines a high-temperature period; outside these slots, valves are set to a fallback low temperature.
- **Zigbee <-> WiFi Time Sync**: Connects to WiFi initially to synchronize system time via **SNTP** and fetch the latest configuration.
- **Radio Sharing Optimization**: Automatically shuts down WiFi after synchronization to minimize interference and free up the 2.4GHz radio for the Zigbee stack.
- **Manual Control**: The physical **BOOT button** on the ESP32-C6 acts as a global override, allowing you to toggle all connected valves between High and Low temperatures manually.
- **Auto-Pairing & Identification**: Automatically matches joining Zigbee devices to the configuration using their **IEEE address**.
- **Self-Healing**: Periodically re-sends the current target to every valve and restarts to retry when the configuration or time could not be obtained at boot.

## Hardware Required

- **ESP32-C6** Development Board.
- A status **LED** (default GPIO15, configurable via `menuconfig`) — optional, used for visual feedback.
- One or more **Zigbee TRVs**. Tested with the **Sonoff TRVZB**. Other valves work only if they expose the standard ZCL Thermostat cluster (`0x0201`) on endpoint 1; Tuya-based valves (including many Moes models) use a proprietary cluster and are **not** supported.
- WiFi environment (WPA/WPA2) with internet access for SNTP and JSON fetching.

## Development Environment Setup

This project requires the Espressif IoT Development Framework [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/index.html), **v5.3 or a later 5.x release** (CI builds with `release-v5.3`). Follow the official installation steps.

> **ESP-IDF 6 is not supported yet**: it requires esp-zigbee-lib 2.x, which replaces the `esp_zb_*` API used here with a new `ezb_*` API (and changes the Zigbee storage partition), so it needs a port of the Zigbee layer.

The Zigbee libraries (`esp-zboss-lib`, `esp-zigbee-lib` 1.6.x) are downloaded automatically by the IDF Component Manager on the first build.

Typical setup on Ubuntu/Debian (see the official guide for other systems):

```bash
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
cd <path-to>/esp-idf
git submodule update --init --recursive
./install.sh esp32c6
. ./export.sh        # required in every new shell
```

### Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `"cmake" must be available on the PATH` | Install the system prerequisites above (`cmake`, `ninja-build`, ...). |
| `xtensa-esp32-elf-gcc ... not found` | The target is still `esp32`: a previous `idf.py set-target esp32c6` failed (e.g. because of the missing `cmake`). Run `idf.py fullclean && idf.py set-target esp32c6`. |
| `git submodule update --init --recursive` suggested by CMake, or `ESP-IDF vX-dirty` | ESP-IDF submodules are out of sync: run that command inside the `esp-idf` directory. |
| `JSON download failed` with an `esp-tls` / certificate error | The server certificate is not signed by a public CA (or the clock is not synchronized). See the *HTTPS note*. |

## Software Configuration

### 1. Secrets

Copy `main/secrets.example` to `main/secrets.h` (it is git-ignored) and fill in:

| Define        | Description                                                              |
|---------------|--------------------------------------------------------------------------|
| `WIFI_SSID`   | WiFi network name.                                                       |
| `WIFI_PASS`   | WiFi password.                                                           |
| `BASE_URL`    | Base URL of the repository hosting `config.json`, without trailing `/`.  |
| `DEVICE_NAME` | Name of this coordinator, used as the log tag on the serial monitor.     |

### Configuration URL

The firmware downloads the configuration from:

```
<BASE_URL>/raw/branch/main/config.json
```

This is the **Gitea** raw-file layout, so `BASE_URL` is typically the repository URL, e.g. `http://gitea.local:3000/user/heating-config`. Other hosts (GitHub, a plain web server, …) use different paths: adjust `CONFIG_JSON_URL` in `main/ha_valve_regulator.c` accordingly.

> **HTTPS note**: HTTPS URLs are supported and the server certificate is validated against the ESP-IDF certificate bundle (Mozilla root CAs), so any site with a certificate from a public CA (e.g. Let's Encrypt) works out of the box. Self-signed certificates and private CAs are **rejected**: use plain HTTP on a trusted local network in that case. Validation needs a correct clock, which is why the time is synchronized before the download.

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

| Field         | Description                                                                                                  |
|---------------|--------------------------------------------------------------------------------------------------------------|
| `name`        | Display name, max 31 characters (longer names are truncated).                                                |
| `ieee`        | 64-bit IEEE address as 16 hex digits (`0x` prefix and `:` separators allowed). Use `"-1"` if not known yet: the address of an unknown joining valve is printed on the serial monitor. |
| `enabled`     | `true` to control the valve. **Defaults to `false`** if omitted.                                             |
| `temp_high`   | Temperature inside the slots, in 1/100 °C (e.g. `2100` = 21.00 °C). Default `2100`.                          |
| `temp_low`    | Temperature outside the slots, in 1/100 °C. Default `1600`.                                                  |
| `schedule`    | List of `"HH:MM"` slots; `start` is inclusive, `end` exclusive. A slot may cross midnight (e.g. `23:00`–`06:00`). The same schedule applies every day. |

Limits and validation:
- Max **8 devices** and **8 slots per device**; extra entries are ignored.
- Max file size **4 KB**; a larger file is rejected.
- Temperatures outside **500–3500** (5–35 °C) are rejected and the default is kept.
- Slots with an invalid time are skipped; unknown fields are ignored.

## Build and Flash

1. Set the target to ESP32-C6:
   ```bash
   idf.py set-target esp32c6
   ```
2. Build and flash the firmware:
   ```bash
   idf.py build flash monitor
   ```

## Development

- **Tunable settings**: exposed via `idf.py menuconfig` under *Valve Regulator Configuration*
  (defined in `main/Kconfig.projbuild`). The defaults work out of the box:

  | Option                            | Default                        | Description                                                  |
  |-----------------------------------|--------------------------------|--------------------------------------------------------------|
  | `VALVE_LED_GPIO`                  | 15                             | Status LED pin.                                              |
  | `VALVE_BUTTON_GPIO`               | 9                              | Manual override (BOOT) button pin.                           |
  | `VALVE_ZB_ENDPOINT`               | 1                              | Coordinator endpoint.                                        |
  | `VALVE_ZB_MAX_CHILDREN`           | 10                             | Max joined devices.                                          |
  | `VALVE_ZB_SCAN_ALL_CHANNELS`      | y                              | Scan all channels at network formation (else fixed channel). |
  | `VALVE_ZB_PRIMARY_CHANNEL`        | 13                             | Fixed channel, when scanning is disabled.                    |
  | `VALVE_PERMIT_JOIN_SECONDS`       | 30                             | Pairing window after each boot.                              |
  | `VALVE_RESYNC_MINUTES`            | 30                             | Periodic re-send of the current target (0 = off).            |
  | `VALVE_DEGRADED_RESTART_MINUTES`  | 30                             | Restart delay after a failed config/SNTP boot (0 = never).   |
  | `VALVE_NTP_SERVER`                | `pool.ntp.org`                 | SNTP server.                                                 |
  | `VALVE_TIMEZONE`                  | `CET-1CEST,M3.5.0,M10.5.0/3`   | POSIX timezone string.                                       |

- **Host unit tests**: The portable parsing/scheduling logic (`main/config_parser.c`)
  is unit-tested off-target — no ESP-IDF required:
  ```bash
  cmake -S test/host -B build_host
  cmake --build build_host
  ctest --test-dir build_host --output-on-failure
  ```
- **Formatting**: Code style is enforced with `clang-format` 15 (config in `.clang-format`;
  the vendored `main/jsmn.h` is excluded). Check with:
  ```bash
  clang-format --dry-run --Werror main/*.c main/*.h   # except jsmn.h
  ```
- **CI**: `.github/workflows/ci.yml` runs the firmware build (esp32c6), host tests and
  the format check on every push and pull request.

## Initialization Sequence

1. **NVS Init**: Prepares internal storage.
2. **WiFi Connect**: Connects to the configured SSID (waits up to 20 s).
3. **SNTP Sync**: Synchronizes the internal clock with network time (waits up to 30 s). This comes first because HTTPS certificate validation needs a valid clock.
4. **HTTP(S) Download**: Fetches the TRV configuration JSON.
5. **WiFi Shutdown**: De-initializes WiFi components to optimize Zigbee performance.
6. **Zigbee Start**: Initializes the Zigbee Coordinator (forming a new network on first boot) and opens the network for pairing.

If the SNTP sync or the download fails (an HTTPS download also fails when the clock is not synchronized), the coordinator still starts, but the schedule stays suspended until a restart (see *Re-sync & recovery* below).

## Usage

- **Pairing**: Put your TRV into pairing mode within the pairing window. Once it joins the network, the ESP32-C6 will match its IEEE address against the JSON config. If found, enabled and the time is synchronized, it will immediately apply the current target temperature. Unknown valves are logged with the `ieee` value to add to the config.
- **Monitoring**: Use the serial monitor (exit with `Ctrl+]`) to view current time, connected devices, and temperature updates (a status summary is printed every 5 minutes).
- **Manual Override**: Press the **BOOT button** to force all valves to "HIGH" temperature mode. Press again to force them to "LOW" mode. The scheduled automation will resume at the next time slot transition.
- **Re-sync & recovery**: The current target is re-sent to every connected valve at startup and every 30 minutes (`VALVE_RESYNC_MINUTES`), so a lost Zigbee write or a coordinator reboot is corrected automatically. If the config download or SNTP fails at boot, the schedule is suspended (valves are left untouched) and the board restarts after 30 minutes (`VALVE_DEGRADED_RESTART_MINUTES`) to retry.
- **LED feedback**:
  - N blinks after WiFi/config: N devices loaded from the JSON.
  - 1 long blink (1 s): time synchronized.
  - N blinks at Zigbee start: N previously paired devices restored.
  - Fast double blink: a joining valve matched the config.

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

### Third-Party Components
This project uses the following third-party components:
- **[jsmn](main/jsmn.h)**: Minimal JSON tokenizer (MIT License).
- **[switch_driver](main/switch_driver.c)**: Button driver from the Espressif Zigbee examples (Espressif license, see file header; note its restrictions on binary redistribution).
- **espressif__esp-zboss-lib**: Zigbee stack (Custom DSR/Espressif License).
- **espressif__esp-zigbee-lib**: ESP-Zigbee SDK (Apache License 2.0).
