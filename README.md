# Telemetry

Telemetry is an IoT monitoring system designed to collect environmental and water-related sensor data from an ESP32-S3, transport the data securely over MQTT, validate incoming telemetry, store time-series data, and visualize the results.

The project combines embedded firmware, secure device communication, backend services, data validation, storage, and visualization into one end-to-end IoT system.

## System Overview

The system follows this general data flow:

```text
Physical Sensors
      │
      ▼
   ESP32-S3
      │
      │ MQTT over TLS
      ▼
   Mosquitto
      │
      ▼
Telemetry Validation Service
      │
      ├── Validated telemetry
      │
      └── Rejected telemetry
      │
      ▼
   Node-RED( Only for visualization)
      │
      ▼
   Telegraf
      │
      ▼
   InfluxDB
      │
      ▼
   Grafana
```

The ESP32-S3 reads connected sensors and periodically publishes telemetry to the MQTT broker.

The backend services run in Docker containers and handle communication, validation, processing, storage, and visualization.

## Hardware

The current implementation uses:

* ESP32-S3 development board
* SHT31 temperature and humidity sensor
* DS18B20 temperature sensor for water temperature
* DS18B20 temperature sensor for external temperature
* Breadboard and supporting connections

Detailed hardware information is available in:

```text
docs/hardware-selection.md
```

## Software and Services

### ESP32-S3 Firmware

The firmware is developed using **ESP-IDF**, Espressif's official development framework for ESP32 devices.

The firmware is responsible for:

* Wi-Fi connectivity
* Sensor initialization
* Sensor readings
* Device identification
* Telemetry generation
* MQTT communication
* TLS certificate validation
* Periodic telemetry publishing

### Mosquitto

Eclipse Mosquitto provides the MQTT broker used for communication between the ESP32-S3 and backend services.

The broker is configured with:

* Authentication
* Access Control Lists (ACLs)
* TLS encryption
* Separate permissions for different services

The secure MQTT listener uses:

```text
8883
```

### Telemetry Validation Service

A Python validation service receives raw telemetry from MQTT before the data is allowed to continue through the system.

Validation includes checks for:

* Message structure
* Device ID
* Sensor type
* Data type
* Sensor status
* Plausible sensor values
* Required metadata

Sensors are validated independently.

A failed sensor reading therefore does not automatically cause valid readings from other sensors in the same telemetry cycle to be rejected.

Validated and rejected telemetry are published to separate MQTT topics.

### Node-RED

Node-RED consumes validated telemetry and handles backend data flows.

Only telemetry that has passed the validation service is forwarded through the normal processing pipeline.

### Telegraf

Telegraf collects processed telemetry and writes the measurements to InfluxDB.

### InfluxDB

InfluxDB provides time-series storage for sensor measurements.

The current Docker environment uses:

```text
InfluxDB 2.8
```

The default project configuration uses:

```text
Organization: telemetry
Bucket: telemetry
```

### Grafana

Grafana provides dashboards for inspecting and visualizing stored telemetry.

## Repository Structure

The repository is organized around firmware, backend services, tests, configuration, and documentation.

```text
.
├── firmware/
│   └── esp32s3/
│
├── src/
│   └── telemetry_service/
│
├── tests/
│   └── telemetry_service/
│
├── docker/
│
├── docs/
│
├── compose.yaml
│
└── README.md
```

The exact structure may evolve as the system is developed.

## Requirements

### Backend

To run the backend services, install:

* Git
* Docker
* Docker Compose

Docker Desktop can be used on operating systems where it is available.

The services themselves run inside Docker containers, so the project is not tied to a specific host operating system.

### ESP32-S3 Firmware

ESP32-S3 development requires **ESP-IDF**.

Espressif provides installation methods for:

* Windows
* Linux
* macOS

Use Espressif's official ESP-IDF installation documentation and select the setup instructions appropriate for your operating system.

The ESP-IDF installation includes the required compiler toolchain, build tools, Python environment, and ESP32 development utilities.

After installation, verify that ESP-IDF is available:

```bash
idf.py --version
```

## Clone the Repository

Clone the repository and enter the project directory:

```bash
git clone <repository-url>
cd Micro2
```

## Backend Setup

The backend services are managed using Docker Compose.

Configuration files, credentials, certificates, and environment variables required by the services must be prepared before starting the complete stack.

Start the services with:

```bash
docker compose up -d
```

Check their status with:

```bash
docker compose ps
```

View logs with:

```bash
docker compose logs
```

To follow logs continuously:

```bash
docker compose logs -f
```

Stop the environment with:

```bash
docker compose down
```

## MQTT Security

MQTT communication is protected using TLS.

The system uses a local Certificate Authority to validate the MQTT broker certificate.

The broker configuration includes:

* TLS listener
* Server certificate
* Server private key
* CA certificate
* Username/password authentication
* MQTT ACL rules

Anonymous MQTT access is disabled.

Different system components receive only the MQTT permissions they require.

For example:

```text
ESP32 device
    │
    └── publish raw telemetry

Telemetry validator
    │
    ├── read raw telemetry
    └── publish validated/rejected telemetry

Backend consumers
    │
    └── read approved telemetry
```

Certificates, private keys, passwords, tokens, and other secrets must not be committed to Git.

## Configure the ESP32-S3

Enter the firmware directory:

```bash
cd firmware/esp32s3
```

Open the ESP-IDF project configuration:

```bash
idf.py menuconfig
```

Configure the required project settings, including the applicable:

* Wi-Fi credentials
* MQTT broker hostname
* MQTT username
* MQTT password
* Device ID
* Publishing interval
* Sensor configuration
* GPIO configuration
* DS18B20 sensor addresses

The firmware uses the CA certificate included with the project to verify the MQTT broker during the TLS connection.

## Build the Firmware

From the ESP32-S3 firmware directory:

```bash
idf.py build
```

A successful build confirms that the firmware and its dependencies can be compiled for the target device.

## Flash the ESP32-S3

Connect the ESP32-S3 to the computer using USB.

Flash the firmware with:

```bash
idf.py flash
```

The exact serial port handling is managed according to the ESP-IDF environment and operating system being used.

## Monitor the ESP32-S3

Serial output can be inspected with:

```bash
idf.py monitor
```

The monitor can be used to inspect:

* Device startup
* Wi-Fi connection
* Sensor initialization
* Sensor readings
* MQTT connection
* TLS connection
* Published telemetry
* Runtime errors

Exit the monitor using the ESP-IDF monitor exit command.

## Telemetry Flow

A normal telemetry cycle follows this path:

```text
Sensors
   │
   ▼
ESP32-S3
   │
   │ Raw telemetry
   ▼
MQTT Broker
   │
   ▼
Telemetry Validator
   │
   ├── Valid
   │      │
   │      ▼
   │   Validated MQTT topics
   │      │
   │      ▼
   │   Node-RED / Telegraf
   │      │
   │      ▼
   │   InfluxDB
   │      │
   │      ▼
   │   Grafana
   │
   └── Invalid
          │
          ▼
      Rejected MQTT topics
```

This separation prevents invalid sensor data from entering the normal telemetry pipeline.

## Sensor Handling

The ESP32-S3 currently collects measurements including:

* Internal/environment temperature
* Humidity
* Water temperature
* External temperature

Sensor readings are transmitted as structured telemetry messages.

The backend validates each sensor independently so that one failed or unavailable sensor does not prevent valid measurements from other sensors from being processed.

For the complete telemetry format and validation rules, see:

```text
docs/data-contract.md
```

## Tests

The telemetry validation service includes automated Python tests.

Run the tests from the project environment with:

```bash
pytest
```

The tests cover validation behaviour including:

* Valid telemetry
* Invalid metadata
* Invalid sensor values
* Sensor status failures
* Independent sensor validation
* Accepted and rejected telemetry

## Documentation

Additional project documentation is available under:

```text
docs/
```

Important documents include:

```text
docs/hardware-selection.md
docs/system-architecture.md
docs/data-contract.md
docs/docker-core-services.md
docs/node-red-import.md
docs/influxdb-grafana.md
```

These documents contain more detailed information about individual parts of the system.

## Security

The project uses several measures to reduce unnecessary access between components:

* MQTT traffic is encrypted using TLS.
* Anonymous MQTT access is disabled.
* MQTT users authenticate using credentials.
* ACL rules restrict MQTT topic access.
* Devices cannot freely access backend MQTT topics.
* Backend services receive only the permissions they require.
* Sensitive certificates, private keys, passwords, and tokens are excluded from version control.

This security model is designed for the scope of the project and is not intended to represent a complete production security architecture.
