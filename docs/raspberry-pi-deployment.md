# Raspberry Pi Deployment

## Status

Deferred.

The active Telemetry development environment runs on the Mac. The Raspberry Pi Zero 2W is not currently used as the Docker host because its memory is insufficient for the complete six-service stack.

This deployment target has not been abandoned. It may be reconsidered with a reduced service profile or more capable Raspberry Pi hardware.

## Earlier verification

The following was successfully verified on the Raspberry Pi Zero 2W:

- 64-bit ARM operating system
- Docker Engine
- Docker Compose
- Mosquitto container
- Python telemetry-service container
- Node-RED container
- Authenticated MQTT communication
- Access to the Node-RED editor from another computer on the local network
- Raw telemetry validation and validated MQTT output

The Pi reported approximately 416 MiB of usable memory. Running Mosquitto, the Python service and Node-RED left too little capacity for confidently adding Telegraf, InfluxDB and Grafana.

## Current limitation

The current `compose.yaml` defines:

- Mosquitto
- Python telemetry service
- Node-RED
- Telegraf
- InfluxDB
- Grafana

Do not deploy the complete current Compose stack to the Pi Zero 2W without first measuring its resource requirements and creating a Pi-specific service profile or override.

## Options for future Pi deployment

### Reduced edge deployment

Run only:

- Mosquitto
- Python telemetry service
- Node-RED, if its visual flow is required at the edge

Run Telegraf, InfluxDB and Grafana on the Mac or another computer.

### More capable Raspberry Pi

A Pi with more memory may be able to host the complete stack, but this must be verified through load and stability testing.

### Mac-only deployment

Continue running all services on the Mac. The ESP32-S3 connects to the Mac’s local network address.

This is the current implementation.

## Checks required before reactivation

Verify the Pi:

```bash
uname -m
free -h
df -h /
docker --version
docker compose version
```

Expected architecture:

```text
aarch64
```

Check whether another Mosquitto installation already owns port `1883`:

```bash
sudo systemctl status mosquitto --no-pager
sudo ss -tulpn | grep ':1883'
```

If a native Mosquitto service conflicts with the Docker broker, it can be disabled:

```bash
sudo systemctl disable --now mosquitto
```

Do not uninstall it unless removal is intentionally required.

## Required implementation work

Before using the Pi again:

1. Create a Pi-specific Compose profile or override.
2. Decide which services run on the Pi and which remain on the Mac.
3. Recreate local credentials on the deployment host.
4. Verify that secret and password files remain ignored by Git.
5. Measure memory, CPU, temperature and storage usage.
6. Run an extended telemetry and reconnect test.
7. Confirm the services recover after a Pi restart.
8. Update the active architecture documentation.

General Docker setup, credentials and service commands are maintained in [`docker/README.md`](../docker/README.md) and should not be duplicated here.

## Persistent data

Stopping containers without deleting volumes uses:

```bash
docker compose down
```

Do not use `--volumes` unless deletion of stored development data is intentional.

## Security

Any Pi deployment remains limited to a trusted local network until TLS, MQTT topic ACLs and application authentication are implemented.

Do not expose Mosquitto, Node-RED, InfluxDB or Grafana directly through router port forwarding.
