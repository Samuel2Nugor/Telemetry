# Architecture Decision Log

This file records decisions that materially affect the Telemetry architecture. Operational instructions belong in [`docker/README.md`](../docker/README.md), message formats belong in the [data contract](data-contract.md), and current topology belongs in the [system architecture](system-architecture.md).

## ADR-001: Use Python for telemetry validation

**Date:** 2026-09-09
**Status:** Accepted and implemented

### Context

Raw ESP32-S3 telemetry requires metadata validation, independent validation of each sensor reading, UTC timestamp assignment and publication of validated or rejected results.

Implementing this logic inside Node-RED would place core backend behaviour in JavaScript function nodes and make automated testing more difficult.

### Decision

Use a dedicated Python telemetry service as the validation and timestamp authority.

Keep Node-RED outside the validation path.

### Consequences

- Validation remains in normal Python source files.
- Validation rules can be covered by automated tests.
- Each sensor can be accepted or rejected independently.
- Node-RED can be changed or removed without changing validation behaviour.
- The service requires its own MQTT identity and Docker container.
- The validator remains stateless.

### Alternatives considered

**Node-RED validation:** rejected because core validation would depend on visual flows and JavaScript function nodes.

**ESP32-only validation:** rejected because basic hardware checks on the device do not remove the need to validate incoming backend data.

## ADR-002: Use the Mac as the active Docker host

**Date:** 2026-09-12
**Status:** Accepted and implemented

### Context

The initial design placed Mosquitto, the Python telemetry service and Node-RED on a Raspberry Pi Zero 2W.

Testing showed that the Pi could run the three-container stack, but its approximately 416 MiB of usable memory left little capacity for reliable expansion. Adding InfluxDB, Grafana and Telegraf would increase resource pressure and operational complexity.

The development Mac already contains the repository, local credentials and Docker environment and remains powered while telemetry is being collected.

### Decision

Run the complete development stack on the Mac.

Keep Raspberry Pi deployment as an optional deferred target rather than an active requirement.

### Consequences

- All six services can run on one Docker Compose network.
- InfluxDB and Grafana can run without Pi memory pressure.
- The ESP32-S3 must connect to the Mac’s local network address.
- Telemetry collection stops when Docker or the Mac is stopped.
- A future Pi deployment requires separate resource testing and may require a reduced Compose profile.

### Alternative considered

**Continue using the Pi as the primary host:** rejected for the current phase because the resource margin is too small for the full stack.

## ADR-003: Store validated MQTT telemetry with Telegraf and InfluxDB

**Date:** 2026-09-12
**Status:** Accepted and implemented

### Context

MQTT transports messages but does not provide historical queries. Grafana requires a queryable data source, and the Python validator should remain focused on validation rather than database-writing responsibilities.

### Decision

Use Telegraf to subscribe to validated MQTT topics and write measurements to InfluxDB.

Use Grafana to query InfluxDB and display telemetry history.

### Consequences

- The Python validator remains independent of InfluxDB.
- Node-RED and Telegraf consume validated MQTT messages in parallel.
- Historical measurements remain available after MQTT messages are gone.
- Telegraf configuration defines how validated JSON fields become InfluxDB fields and tags.
- InfluxDB, Telegraf and Grafana add three containers to the environment.
- The current development configuration uses the InfluxDB administrative token; separate least-privilege tokens remain future work.

### Version decision

InfluxDB `2.9.0` was tested but failed during Docker initialization because of an entrypoint and `dasel` compatibility problem.

InfluxDB is therefore pinned to `2.8.0`, which initialized successfully and passed its healthcheck.

Telegraf is pinned to `1.40.0`, and Grafana is pinned to `12.4.0`.

## ADR-004: Provision Grafana configuration from Git

**Date:** 2026-09-13
**Status:** Accepted and implemented

### Context

A dashboard created only through the Grafana interface exists inside the Grafana data volume. Losing that volume or setting up another development computer would require manually rebuilding the datasource and panels.

### Decision

Store the Grafana datasource, dashboard provider and exported Telemetry dashboard as version-controlled provisioning files.

### Consequences

- Grafana can recreate the datasource and dashboard automatically.
- Dashboard structure is reviewable in Git.
- Secrets remain supplied through environment variables and are not stored in dashboard JSON.
- Changes made through the Grafana interface must be exported again if they should become the version-controlled definition.

## Pending decisions

The following items are intentionally unresolved:

- Where sequence-gap and duplicate detection should run
- How silent-hang detection should distinguish a wedged service from an intentionally offline ESP32-S3
- Whether a watchdog should terminate the telemetry process to trigger Docker restart
- How MQTT topic ACLs should be divided between service accounts
- How separate InfluxDB read and write tokens should be scoped

## ADR-005: Suppress recent duplicate telemetry in the Python service

**Date:** 2026-09-17

**Status:** Accepted and implemented

### Decision

Use `(device_id, boot_id, sequence)` to identify repeated measurement cycles.
The Python telemetry service keeps up to 4,096 recent identities in memory
and drops duplicates during the same service run.

### Consequences

- Recent MQTT redeliveries do not create repeated validated measurements.
- The cache resets on restart; persistent deduplication is not guaranteed.
- Sequence-gap detection remains future work.

## ADR-006: Use DS18B20 for external temperature

**Date:** 2026-09-18

**Status:** Accepted; hardware integration pending

### Decision

Use one internal SHT31 for temperature and humidity, one DS18B20 for
external air temperature, and a second DS18B20 for water temperature.
Keep the measurement names; change the external sensor ID to
`external_ds18b20`.

### Consequences

- The data contract, validator, tests and hardware documentation use the new ID.
- Firmware must map each DS18B20's unique serial code to its physical location.
- Wiring and physical sensor reads still need verification.
