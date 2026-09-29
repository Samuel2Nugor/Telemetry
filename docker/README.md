# Docker configuration

This directory contains the configuration for the six Telemetry backend services. The ESP32-S3 sends readings from the physical sensors every five seconds by default; the backend validates them and stores accepted measurements. See the [root README](../README.md) for startup, shutdown, service addresses and telemetry verification. See the [data contract](../docs/data-contract.md) for MQTT payloads and validation rules.

## Configuration files

| Path | Purpose |
| ---- | ------- |
| `mosquitto/config/mosquitto.conf` | Broker listener, authentication and TLS |
| `mosquitto/config/acl_file` | MQTT topic permissions |
| `node-red/flows/validated-telemetry.json` | Exported flow for inspecting validated telemetry |
| `telegraf/telegraf.conf` | MQTT consumption and InfluxDB output |
| `grafana/provisioning/` | Data source and dashboard provisioning |
| `grafana/dashboards/` | Dashboard definitions |

Run the following setup commands from the repository root. The examples use a POSIX-compatible shell.

## MQTT accounts

The broker uses these accounts:

- `esp32s3-01`
- `telemetry-service`
- `node-red`
- `telegraf`

Create the password file with the first account:

```bash
docker run --rm -it \
  -v "$PWD/docker/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2.1.2-alpine \
  mosquitto_passwd -c /mosquitto/config/password_file esp32s3-01
```

Add each remaining account with the same command **without `-c`**, replacing `<service-name>` with `telemetry-service`, `node-red` or `telegraf`:

```bash
docker run --rm -it \
  -v "$PWD/docker/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2.1.2-alpine \
  mosquitto_passwd /mosquitto/config/password_file <service-name>
```

`-c` creates or overwrites the password file, so use it only for the first account. Keep the file outside Git.

## Environment

Create `.env` from `.env.example` if needed. Configure the MQTT settings for the Compose services:

```dotenv
MQTT_HOST=mosquitto
MQTT_PORT=8883
MQTT_CA_CERT=/etc/telemetry/certs/ca.crt
MQTT_USERNAME=telemetry-service
TELEGRAF_MQTT_USERNAME=telegraf
```

Set `MQTT_PASSWORD` and `TELEGRAF_MQTT_PASSWORD` to match their password-file accounts. Configure ESP32-S3 and Node-RED credentials in those clients. `.env` also contains InfluxDB and Grafana setup values; keep it outside Git.

## TLS certificates

Provision these files locally before starting the services:

| Path | Purpose |
| ---- | ------- |
| `docker/mosquitto/certs/ca.crt` | Public CA certificate shared with MQTT clients |
| `docker/mosquitto/certs/server.crt` | Broker certificate |
| `docker/mosquitto/certs/server.key` | Broker private key |

The broker certificate's Subject Alternative Names must cover the names clients use: `<broker-hostname>.local` for the device, `mosquitto` for containers and `localhost` for local checks, plus IP address `127.0.0.1`.

Mosquitto mounts the certificate directory at `/mosquitto/certs`. Backend MQTT clients mount the public CA certificate at `/etc/telemetry/certs/ca.crt`. Keep the CA private key outside containers. Docker Compose does not create or renew certificates.

MQTT listens on port `8883` with TLS. The plaintext listener on `1883` is disabled.

## Topic permissions

| Account | Allowed operations |
| ------- | ------------------ |
| `esp32s3-01` | Publish its own raw telemetry and status |
| `telemetry-service` | Read raw telemetry; publish validated, rejected and broker health messages |
| `node-red` | Read validated telemetry and device status |
| `telegraf` | Read validated telemetry |

Other operations are denied. The status permissions reserve a topic for a future device-status feature; current firmware has not been verified publishing online/offline status. Reading rejected telemetry requires an explicitly authorized diagnostic account or ACL change.

## Broker healthcheck

The broker healthcheck publishes to `telemetry/health/mosquitto` using TLS, MQTT v5 and QoS 1. It reuses the telemetry-service credentials. An accepted publish may report MQTT reason code `0` or `16` (no subscriber).

The healthcheck confirms that the broker accepts a publish; use the verification steps in the root README to check the complete sensor-to-dashboard flow.

## Node-RED flow

Import `docker/node-red/flows/validated-telemetry.json` in the Node-RED editor, then configure:

- Broker: `mosquitto:8883`
- TLS: enabled, with broker certificate verification
- CA certificate: `/etc/telemetry/certs/ca.crt`
- Server name: `mosquitto`
- MQTT username and password: the `node-red` account

The flow subscribes to `telemetry/v1/devices/+/telemetry/validated/+` at QoS `1` and displays the decoded messages in Debug. Credentials are not included in the exported flow. Deploy it after configuring the broker, and export changes made in the editor back to the repository flow file.

## Telegraf and InfluxDB

Telegraf subscribes to validated telemetry through `ssl://mosquitto:8883`, verifies the broker certificate and writes measurements to the InfluxDB `telemetry` bucket.

InfluxDB initialization values in `.env` apply only when its data volume is empty. Changing the initialization password or token later does not update existing credentials.

## Security scope

MQTT TLS protects the MQTT connections. The HTTP interfaces and Telegraf's HTTP connection to InfluxDB are not covered by it. Node-RED editor authentication and separate restricted InfluxDB tokens for services remain unconfigured; Telegraf and Grafana currently use the configured InfluxDB admin token. Keep development services on a trusted local network.
