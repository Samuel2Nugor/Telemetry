import json
import math
from typing import Any


MEASUREMENT_CONFIG = {
    "internal_temperature_c": {
        "measurement": "internal_temperature",
        "sensor_id": "internal_sht31",
        "unit": "celsius",
        "minimum": -10.0,
        "maximum": 60.0,
    },
    "internal_humidity_percent": {
        "measurement": "internal_humidity",
        "sensor_id": "internal_sht31",
        "unit": "percent_rh",
        "minimum": 0.0,
        "maximum": 100.0,
    },
    "external_temperature_c": {
        "measurement": "external_temperature",
        "sensor_id": "external_ds18b20",
        "unit": "celsius",
        "minimum": -40.0,
        "maximum": 60.0,
    },
    "water_temperature_c": {
        "measurement": "water_temperature",
        "sensor_id": "water_ds18b20",
        "unit": "celsius",
        "minimum": 0.0,
        "maximum": 50.0,
    },
}

STATUS_REASON_CODES = {
    "read_error": "sensor_read_error",
    "not_detected": "sensor_not_detected",
    "invalid_value": "invalid_value",
}

SUPPORTED_SCHEMA_VERSION = 1

REQUIRED_METADATA_FIELDS = {
    "schema_version",
    "device_id",
    "boot_id",
    "sequence",
    "uptime_ms",
    "measurements",
    "sensor_status",
}

def validate_raw_payload(
    raw_payload: bytes | str,
    *,
    topic_device_id: str,
    timestamp: str,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if not isinstance(raw_payload, (bytes, str)) or not raw_payload.strip():
        rejection = _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="empty_payload",
            description="The MQTT payload is empty",
        )
        return [], [rejection]

    try:
        payload = json.loads(raw_payload)
    except (json.JSONDecodeError, UnicodeDecodeError):
        rejection = _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_json",
            description="The MQTT payload is not valid UTF-8 JSON",
        )
        return [], [rejection]

    if not isinstance(payload, dict):
        rejection = _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="The JSON payload must be an object",
        )
        return [], [rejection]

    return validate_raw_message(
        payload,
        topic_device_id=topic_device_id,
        timestamp=timestamp,
    )


def validate_raw_message(
    payload: dict[str, Any],
    *,
    topic_device_id: str,
    timestamp: str,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    metadata_rejection = _validate_metadata(
        payload=payload,
        topic_device_id=topic_device_id,
        timestamp=timestamp,
    )

    if metadata_rejection is not None:
        return [], [metadata_rejection]

    validated: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []

    measurements = payload["measurements"]
    sensor_status = payload["sensor_status"]

    for raw_field, config in MEASUREMENT_CONFIG.items():
        sensor_id = config["sensor_id"]

        if raw_field not in measurements:
            rejected.append(
                _build_rejection(
                    payload=payload,
                    timestamp=timestamp,
                    measurement=config["measurement"],
                    sensor_id=sensor_id,
                    received_value=None,
                    reason_code="missing_measurement",
                    description=f"Required measurement field is missing: {raw_field}",
                )
            )
            continue

        value = measurements[raw_field]

        if sensor_id not in sensor_status:
            rejected.append(
                _build_rejection(
                    payload=payload,
                    timestamp=timestamp,
                    measurement=config["measurement"],
                    sensor_id=sensor_id,
                    received_value=value,
                    reason_code="missing_sensor_status",
                    description=f"Required sensor status is missing: {sensor_id}",
                )
            )
            continue

        status = sensor_status[sensor_id]

        if status != "ok":
            reason_code = STATUS_REASON_CODES.get(
                status,
                "invalid_sensor_status",
            )

            rejected.append(
                _build_rejection(
                    payload=payload,
                    timestamp=timestamp,
                    measurement=config["measurement"],
                    sensor_id=sensor_id,
                    received_value=value,
                    reason_code=reason_code,
                    description=f"Sensor status is {status!r}",
                )
            )
            continue

        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
        ):
            rejected.append(
                _build_rejection(
                    payload=payload,
                    timestamp=timestamp,
                    measurement=config["measurement"],
                    sensor_id=sensor_id,
                    received_value=value,
                    reason_code="invalid_type",
                    description="Measurement must be a finite number",
                )
            )
            continue

        if not config["minimum"] <= value <= config["maximum"]:
            rejected.append(
                _build_rejection(
                    payload=payload,
                    timestamp=timestamp,
                    measurement=config["measurement"],
                    sensor_id=sensor_id,
                    received_value=value,
                    reason_code="out_of_plausible_range",
                    description=(
                        f"Measurement must be between "
                        f"{config['minimum']} and {config['maximum']}"
                    ),
                )
            )
            continue

        validated.append(
            {
                "schema_version": payload["schema_version"],
                "device_id": topic_device_id,
                "boot_id": payload["boot_id"],
                "sequence": payload["sequence"],
                "uptime_ms": payload["uptime_ms"],
                "timestamp": timestamp,
                "measurement": config["measurement"],
                "value": value,
                "unit": config["unit"],
                "sensor_id": sensor_id,
            }
        )

    return validated, rejected

def _validate_metadata(
    *,
    payload: dict[str, Any],
    topic_device_id: str,
    timestamp: str,
) -> dict[str, Any] | None:
    missing_fields = REQUIRED_METADATA_FIELDS - payload.keys()

    if missing_fields:
        fields = ", ".join(sorted(missing_fields))

        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="missing_metadata",
            description=f"Missing required metadata: {fields}",
            payload=payload,
        )

    schema_version = payload["schema_version"]

    if isinstance(schema_version, bool) or not isinstance(schema_version, int):
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="schema_version must be an integer",
            payload=payload,
        )

    if schema_version != SUPPORTED_SCHEMA_VERSION:
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="unsupported_schema",
            description=f"Unsupported schema version: {schema_version}",
            payload=payload,
        )

    device_id = payload["device_id"]

    if not isinstance(device_id, str) or not device_id.strip():
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="device_id must be a non-empty string",
            payload=payload,
        )

    if device_id != topic_device_id:
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="device_id_mismatch",
            description="Payload device_id does not match the MQTT topic",
            payload=payload,
        )

    boot_id = payload["boot_id"]

    if not isinstance(boot_id, str) or not boot_id.strip():
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="boot_id must be a non-empty string",
            payload=payload,
        )

    if not _is_non_negative_integer(payload["sequence"]):
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="sequence must be a non-negative integer",
            payload=payload,
        )

    if not _is_non_negative_integer(payload["uptime_ms"]):
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="uptime_ms must be a non-negative integer",
            payload=payload,
        )

    if not isinstance(payload["measurements"], dict):
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="measurements must be an object",
            payload=payload,
        )

    if not isinstance(payload["sensor_status"], dict):
        return _build_message_rejection(
            topic_device_id=topic_device_id,
            timestamp=timestamp,
            reason_code="invalid_metadata",
            description="sensor_status must be an object",
            payload=payload,
        )

    return None


def _is_non_negative_integer(value: Any) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and value >= 0
    )


def _build_message_rejection(
    *,
    topic_device_id: str,
    timestamp: str,
    reason_code: str,
    description: str,
    payload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    rejection: dict[str, Any] = {
        "schema_version": SUPPORTED_SCHEMA_VERSION,
        "device_id": topic_device_id,
        "timestamp": timestamp,
        "rejection_scope": "message",
        "reason_code": reason_code,
        "description": description,
    }

    # Carry boot_id/sequence through when the source payload has them, even
    # though the message is being rejected, so the backend can still
    # deduplicate this rejection on MQTT QoS 1 redelivery.
    if isinstance(payload, dict):
        boot_id = payload.get("boot_id")
        if isinstance(boot_id, str) and boot_id.strip():
            rejection["boot_id"] = boot_id

        sequence = payload.get("sequence")
        if _is_non_negative_integer(sequence):
            rejection["sequence"] = sequence

    return rejection


def _build_rejection(
    *,
    payload: dict[str, Any],
    timestamp: str,
    measurement: str,
    sensor_id: str,
    received_value: Any,
    reason_code: str,
    description: str,
) -> dict[str, Any]:
    return {
        "schema_version": SUPPORTED_SCHEMA_VERSION,
        "device_id": payload["device_id"],
        "boot_id": payload["boot_id"],
        "sequence": payload["sequence"],
        "timestamp": timestamp,
        "rejection_scope": "measurement",
        "measurement": measurement,
        "sensor_id": sensor_id,
        "received_value": received_value,
        "reason_code": reason_code,
        "description": description,
    }
