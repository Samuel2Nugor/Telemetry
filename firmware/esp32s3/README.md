# Telemetry ESP32-S3 Firmware

This directory contains the ESP-IDF firmware for the Telemetry sensor node. The firmware reads one SHT31 and two waterproof DS18B20 probes, builds raw telemetry, and publishes it to the Telemetry MQTT broker over authenticated TLS.

The current implementation has been tested with ESP-IDF `v6.0.1` on an ESP32-S3 with Wi-Fi, PSRAM and a 16 MB flash device.

## Runtime flow

At startup, the firmware:

1. Initializes NVS.
2. Initializes the SHT31 IÂ²C bus.
3. Initializes the shared DS18B20 1-Wire bus and assigns each known ROM address to a sensor role.
4. Connects to Wi-Fi.
5. Synchronizes UTC time through SNTP.
6. Connects to Mosquitto using TLS and MQTT authentication.
7. Reads the sensors and publishes raw telemetry every configured interval.

The default telemetry interval is `5000 ms`. A single `boot_id` is retained for the lifetime of each boot, while `sequence` increases after every successfully queued MQTT publication.

The raw topic is derived from the configured device ID:

```text
telemetry/v1/devices/<device-id>/telemetry/raw
```

The complete payload definition and validation behavior are documented in [`../../docs/data-contract.md`](../../docs/data-contract.md).

## Hardware connections

The default configuration uses the following wiring:

| Device | Connection | ESP32-S3 |
| ------ | ---------- | -------- |
| SHT31 | VIN | 3.3 V |
| SHT31 | GND | GND |
| SHT31 | SDA | GPIO8 |
| SHT31 | SCL | GPIO9 |
| Both DS18B20 probes | VCC | 3.3 V |
| Both DS18B20 probes | GND | GND |
| Both DS18B20 probes | Data | GPIO5 |

Both DS18B20 probes share the same 1-Wire data line. One `4.7 kÎ©` pull-up resistor must connect the shared data line to 3.3 V. The resistor belongs to the bus, so adding another probe does not require another pull-up resistor.

The default probe-role mapping is:

| Role | DS18B20 ROM address |
| ---- | ------------------- |
| Water temperature | `BE0000006DEFD428` |
| External temperature | `DA00000070118128` |

Pins and ROM addresses can be changed through Kconfig without editing the source files. See [`../../docs/hardware-selection.md`](../../docs/hardware-selection.md) for the broader hardware description.

## Source structure

| File | Responsibility |
| ---- | -------------- |
| `main/main.c` | Startup, periodic sampling, telemetry payload creation and sequencing |
| `main/sht31.c` | SHT31 initialization and temperature/humidity readings |
| `main/ds18b20_manager.c` | 1-Wire setup, ROM discovery, role assignment and DS18B20 readings |
| `main/wifi_manager.c` | Wi-Fi station connection and retries |
| `main/mqtt_publisher.c` | TLS MQTT connection and QoS 1 publication |
| `main/Kconfig.projbuild` | Device, network, MQTT and sensor configuration options |
| `main/idf_component.yml` | Managed ESP-IDF component dependencies |

The DS18B20 implementation uses the Espressif `ds18b20` component with the UART-based `onewire_bus` backend.
The current hardware requires an initial 1-Wire reset before the first ROM search; that workaround is contained inside `ds18b20_manager.c`.

## Configuration

Open the project configuration menu from this directory:

```bash
idf.py menuconfig
```

Open **Telemetry configuration** and set:

- Device ID and telemetry interval
- Wi-Fi SSID and password
- MQTT broker URI, username and password
- SHT31 SDA and SCL GPIOs
- 1-Wire GPIO and UART port
- Water and external DS18B20 ROM addresses

For the tested TLS setup, the broker URI follows this form:

```text
mqtts://<broker-hostname>.local:8883
```

The broker certificate must be trusted by the CA certificate embedded from:

```text
main/certs/mqtt_ca.crt
```

`sdkconfig` and `sdkconfig.old` contain machine-specific settings and may contain credentials. They must remain untracked. Only non-secret shared defaults belong in `sdkconfig.defaults`.

## Build, flash and monitor

Activate the ESP-IDF environment, enter this directory, and run:

```bash
idf.py build
idf.py flash monitor
```

Use the serial port assigned by the operating system if it differs from `/dev/ttyACM0`. Exit the monitor with `Ctrl+]`.

A successful startup should report:

- SHT31 detected and registered
- Two 1-Wire ROM addresses discovered
- `water=ready external=ready`
- Wi-Fi connected
- Network time synchronized
- MQTT broker connected
- Increasing telemetry sequence numbers
- QoS 1 acknowledgements

The sensor-reading timestamps should advance by approximately the configured interval. With the default configuration, consecutive cycles begin five seconds apart.

## Failure behavior

- If an SHT31 read fails, that cycle is skipped and the firmware retries during the next interval.
- If a DS18B20 probe is missing or fails during a read, its value is published as `null` with an appropriate sensor status. The other available measurements can still be validated independently by the telemetry service.
- If MQTT is temporarily disconnected, the publication fails for that cycle without advancing the sequence number. The next interval attempts publication again.

## Current limitation

The current application binary leaves approximately 8% free space in the smallest configured application partition.
New firmware features may require reducing binary size or changing the partition table. Check the reported binary size after every build.
