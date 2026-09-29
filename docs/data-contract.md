# Data Contract

## Purpose and implementation status

This document defines the MQTT topics and JSON payloads exchanged by the Telemetry sensor node, Python telemetry service, Node-RED and Telegraf.
Contract version `v1` uses the numeric JSON field `"schema_version": 1`.

Raw, validated and rejected telemetry are implemented. The ESP32-S3 has published real readings from one SHT31 and two waterproof DS18B20 probes.
In the tested working configuration, the validator accepted all four measurements from each raw message (`validated=4 rejected=0`).
A sensor failure can still produce a measurement rejection without rejecting valid readings from the same message.

The device-status topic and Last Will format later in this document are defined for possible integration; they are **not verified as implemented** by the current firmware.
They must not be used as evidence that online/offline monitoring already works.

## MQTT transport and topics

MQTT clients connect to Mosquitto using TLS on port `8883` and authenticate with an MQTT username and password. The plaintext listener on `1883` is disabled.
Topic ACLs restrict which clients may publish or subscribe. TLS and authentication protect the connection; the following tables define the message contents.

| Topic pattern | Publisher | Consumer | Meaning |
| --- | --- | --- | --- |
| `telemetry/v1/devices/{device_id}/telemetry/raw` | ESP32-S3 | Python telemetry service | Combined sensor readings and statuses |
| `telemetry/v1/devices/{device_id}/telemetry/validated/{measurement}` | Python telemetry service | Node-RED and Telegraf | One accepted measurement |
| `telemetry/v1/devices/{device_id}/telemetry/rejected` | Python telemetry service | Authorized diagnostic clients | A rejected message or measurement |
| `telemetry/v1/devices/{device_id}/status` | Device or broker Last Will, if implemented | Status consumers | Proposed online/offline status |

`{device_id}` identifies one device; the tested prototype uses `esp32s3-01`. `{measurement}` is one of the four names in the validated measurement mapping below. The Python service subscribes to `telemetry/v1/devices/+/telemetry/raw`. Node-RED and Telegraf subscribe to `telemetry/v1/devices/+/telemetry/validated/+`. In MQTT subscriptions, each `+` matches exactly one topic level.

Raw, validated and rejected telemetry use QoS `1`, are **not retained**, and carry UTF-8 JSON. QoS `1` permits redelivery, so consumers should not assume every delivered message is unique. The telemetry service drops recently processed raw cycles using `(device_id, boot_id, sequence)`; this record is in memory and resets when the service restarts. Sequence-gap detection is not implemented.

## Raw telemetry

The ESP32-S3 publishes one combined message to `telemetry/v1/devices/{device_id}/telemetry/raw` per successful measurement cycle. The firmware's current default interval is five seconds (`5000 ms`), configurable through ESP-IDF. The interval is firmware behavior rather than a rule enforced by the JSON validator.

### Example of a successful cycle

Topic:

```text
telemetry/v1/devices/esp32s3-01/telemetry/raw
```

Payload (illustrative values):

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 30,
  "uptime_ms": 185430,
  "measurements": {
    "internal_temperature_c": 23.6,
    "internal_humidity_percent": 61.4,
    "external_temperature_c": 18.9,
    "water_temperature_c": 20.7
  },
  "sensor_status": {
    "internal_sht31": "ok",
    "external_ds18b20": "ok",
    "water_ds18b20": "ok"
  }
}
```

Raw telemetry has no `timestamp` field. The Python service assigns a UTC timestamp when it processes the message.

### Required common fields

| Field | JSON type | Meaning |
| --- | --- | --- |
| `schema_version` | Integer | Must be `1` for this contract |
| `device_id` | String | Must match the device ID in the raw MQTT topic |
| `boot_id` | String | Identifier generated when the device boots |
| `sequence` | Non-negative integer | Counts successfully queued raw publications within one boot |
| `uptime_ms` | Non-negative integer | Milliseconds since the device booted |
| `measurements` | Object | Contains the four fields listed below |
| `sensor_status` | Object | Contains one status per physical sensor |

These fields are required. A missing or invalid common field rejects the entire message. The device creates a new `boot_id` at each boot and starts `sequence` at `0`. The firmware increments `sequence` after a raw publication is queued successfully; an unsuccessful publish attempt does not advance it. The telemetry service preserves the source `boot_id`, `sequence` and `uptime_ms` in accepted messages.

### Required measurements

| Raw field | JSON type | Unit | Physical source |
| --- | --- | --- | --- |
| `internal_temperature_c` | Finite number or `null` | Degrees Celsius | SHT31 |
| `internal_humidity_percent` | Finite number or `null` | Percent relative humidity | SHT31 |
| `external_temperature_c` | Finite number or `null` | Degrees Celsius | External DS18B20 |
| `water_temperature_c` | Finite number or `null` | Degrees Celsius | Water DS18B20 |

All four keys must be present, including when a sensor cannot provide a reading. An unavailable reading is represented by JSON `null`, not by omitting the key or using the string `"null"`.

### Sensor status

| Status field | Affected measurements |
| --- | --- |
| `internal_sht31` | `internal_temperature_c` and `internal_humidity_percent` |
| `external_ds18b20` | `external_temperature_c` |
| `water_ds18b20` | `water_temperature_c` |

| Status value | Meaning |
| --- | --- |
| `ok` | The sensor produced a usable reading |
| `read_error` | The sensor was detected, but a read failed |
| `not_detected` | The expected sensor was not found |
| `invalid_value` | The sensor returned an unusable value |

When a status is not `ok`, every affected measurement must be `null`. Conversely, a `null` reading with an `ok` status is not accepted as valid telemetry. The external temperature status key is `external_ds18b20`; `external_sht31` is not a valid substitute.

If a DS18B20 probe is missing or its read fails, the firmware can still publish the other measurements with `null` and the corresponding DS18B20 error status. In the current firmware, an SHT31 read failure skips that entire publication cycle and retries on the next interval. The validator's independent rejection behavior applies when it receives a raw message containing an invalid measurement.

### Plausibility limits

These are technical limits used by the validator, not desired growing conditions. The endpoints are included in the accepted range.

| Raw field | Minimum | Maximum |
| --- | ---: | ---: |
| `internal_temperature_c` | `-10.0` | `60.0` |
| `internal_humidity_percent` | `0.0` | `100.0` |
| `external_temperature_c` | `-40.0` | `60.0` |
| `water_temperature_c` | `0.0` | `50.0` |

A numeric value outside its range is rejected with `out_of_plausible_range`. Booleans and numeric strings are not valid measurement numbers. Non-finite numbers such as `NaN` or infinity must never appear in telemetry.

## Validated telemetry

For each accepted raw measurement, the Python service publishes one JSON message to `telemetry/v1/devices/{device_id}/telemetry/validated/{measurement}`. A raw message containing four accepted measurements produces four separate validated publications.

### Measurement mapping

| Topic suffix and `measurement` | Raw field | `sensor_id` | `unit` |
| --- | --- | --- | --- |
| `internal_temperature` | `internal_temperature_c` | `internal_sht31` | `celsius` |
| `internal_humidity` | `internal_humidity_percent` | `internal_sht31` | `percent_rh` |
| `external_temperature` | `external_temperature_c` | `external_ds18b20` | `celsius` |
| `water_temperature` | `water_temperature_c` | `water_ds18b20` | `celsius` |

The DS18B20 probes are assigned to water and external roles by their configured ROM addresses. Their specific ROM addresses are hardware configuration and are documented in [hardware selection](hardware-selection.md); ROM addresses are not fields in this MQTT payload.

### Example

Topic:

```text
telemetry/v1/devices/esp32s3-01/telemetry/validated/water_temperature
```

Payload:

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 30,
  "uptime_ms": 185430,
  "timestamp": "2026-09-24T12:00:00Z",
  "measurement": "water_temperature",
  "value": 20.7,
  "unit": "celsius",
  "sensor_id": "water_ds18b20"
}
```

### Required fields

| Field | JSON type | Meaning |
| --- | --- | --- |
| `schema_version` | Integer | Validated-message contract version |
| `device_id` | String | Source device identifier |
| `boot_id` | String | Source boot identifier |
| `sequence` | Integer | Source sequence number |
| `uptime_ms` | Integer | Source uptime in milliseconds |
| `timestamp` | String | UTC timestamp assigned by the Python service, in ISO 8601 format |
| `measurement` | String | Accepted measurement name; matches the topic suffix |
| `value` | Finite number | Accepted value; never `null` |
| `unit` | String | `celsius` or `percent_rh`, according to the mapping |
| `sensor_id` | String | Source sensor identifier, according to the mapping |

Accepted measurements from one raw message share the same `timestamp`, `device_id`, `boot_id`, `sequence` and `uptime_ms`. Node-RED consumes these messages for inspection; Telegraf writes their values to InfluxDB for Grafana to query.

## Rejected telemetry

The Python service publishes failures to `telemetry/v1/devices/{device_id}/telemetry/rejected`. Rejected telemetry is diagnostic and is not written to the normal InfluxDB measurement series. A client that needs to subscribe to rejected topics must have suitable ACL permission.

There are two rejection scopes:

- `message`: the raw message cannot be trusted, so no measurements from it are published as validated.
- `measurement`: one reading fails; other readings from the same raw message can still be published as validated.

### Message rejection example

An empty payload, invalid JSON, unsupported schema version, mismatched device ID or invalid common metadata rejects the complete message. A rejected payload records the reason even when the original input cannot be parsed.

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "timestamp": "2026-09-24T12:00:00Z",
  "rejection_scope": "message",
  "reason_code": "invalid_json",
  "description": "The raw MQTT payload could not be parsed as JSON"
}
```

### Measurement rejection example

If the external probe is absent, the raw message can include `"external_temperature_c": null` with `"external_ds18b20": "not_detected"`. The validator publishes this rejection while accepting valid internal and water measurements.

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 31,
  "timestamp": "2026-09-24T12:00:05Z",
  "rejection_scope": "measurement",
  "measurement": "external_temperature",
  "sensor_id": "external_ds18b20",
  "received_value": null,
  "reason_code": "sensor_not_detected",
  "description": "The external DS18B20 sensor could not be detected"
}
```

### Rejection fields

| Field | JSON type | Present when | Meaning |
| --- | --- | --- | --- |
| `schema_version` | Integer | Every rejection | Rejection format version |
| `device_id` | String | Every rejection | Device ID extracted from the topic |
| `boot_id` | String | Measurement rejection | Source boot session |
| `sequence` | Integer | Measurement rejection | Source sequence number |
| `timestamp` | String | Every rejection | UTC time assigned by the Python service |
| `rejection_scope` | String | Every rejection | `message` or `measurement` |
| `measurement` | String | Measurement rejection | Failed measurement name |
| `sensor_id` | String | Measurement rejection | Sensor associated with the failed reading |
| `received_value` | Any JSON type | Measurement rejection | Value received, including `null` |
| `reason_code` | String | Every rejection | Machine-readable reason |
| `description` | String | Every rejection | Human-readable explanation |

### Reason codes

| Code | Scope | Meaning |
| --- | --- | --- |
| `empty_payload` | Message | MQTT payload is empty |
| `invalid_json` | Message | Payload is not valid JSON |
| `unsupported_schema` | Message | Schema version is unsupported |
| `device_id_mismatch` | Message | Payload device ID differs from the topic device ID |
| `missing_metadata` | Message | Required common metadata is missing |
| `invalid_metadata` | Message | Common metadata has an invalid type or value |
| `missing_measurement` | Measurement | Required measurement field is missing |
| `missing_sensor_status` | Measurement | Required sensor status is missing |
| `invalid_sensor_status` | Measurement | Sensor status is unsupported |
| `invalid_type` | Measurement | Measurement value has an invalid type |
| `sensor_read_error` | Measurement | Sensor reports a read failure |
| `sensor_not_detected` | Measurement | Sensor reports that it was not found |
| `invalid_value` | Measurement | Sensor reports an unusable value |
| `out_of_plausible_range` | Measurement | Numeric value is outside its allowed range |

## Device status: defined, not yet verified

The following is a proposed contract for device availability. The current sensor/telemetry tests do not establish that the ESP32-S3 publishes these messages or configures an MQTT Last Will. Consumers must not assume this topic is active until the firmware implements and verifies it.

| Property | Proposed value |
| --- | --- |
| Topic | `telemetry/v1/devices/{device_id}/status` |
| QoS | `1` |
| Retained | `true` |
| Payload | UTF-8 JSON |

A status subscriber could use `telemetry/v1/devices/+/status`, subject to its MQTT ACL.

Planned online payload, published after MQTT connection:

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "status": "online"
}
```

Planned offline payload, configured as an MQTT Last Will before connecting:

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "status": "offline"
}
```

If this feature is implemented, `status` will be `online` or `offline`; the payload device ID will match its topic; messages will be retained; and the broker will publish the offline Last Will after an unexpected disconnect. A deliberate shutdown would publish `offline` before disconnecting. Telemetry messages remain unretained. Status payloads must never contain credentials.

## General payload rules

Field names are case-sensitive and use `snake_case`. Measurements use JSON numbers, not strings or booleans. `null` represents an unavailable raw reading but is never a validated value. Passwords, Wi-Fi credentials and API tokens must never appear in MQTT payloads.
