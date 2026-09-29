from copy import deepcopy

from telemetry_service.validator import (
    validate_raw_message,
    validate_raw_payload,
)


VALID_PAYLOAD = {
    "schema_version": 1,
    "device_id": "esp32s3-01",
    "boot_id": "a3f82c10",
    "sequence": 42,
    "uptime_ms": 185430,
    "measurements": {
        "internal_temperature_c": 23.6,
        "internal_humidity_percent": 61.4,
        "external_temperature_c": 18.9,
        "water_temperature_c": 20.7,
    },
    "sensor_status": {
        "internal_sht31": "ok",
        "external_ds18b20": "ok",
        "water_ds18b20": "ok",
    },
}

TIMESTAMP = "2026-09-09T12:00:00Z"


def test_valid_raw_message_produces_four_validated_measurements():
    validated, rejected = validate_raw_message(
        VALID_PAYLOAD,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 4
    assert rejected == []

    measurement_names = {item["measurement"] for item in validated}

    assert measurement_names == {
        "internal_temperature",
        "internal_humidity",
        "external_temperature",
        "water_temperature",
    }

    assert all(item["timestamp"] == TIMESTAMP for item in validated)


def test_failed_external_sensor_does_not_block_other_measurements():
    payload = deepcopy(VALID_PAYLOAD)
    payload["measurements"]["external_temperature_c"] = None
    payload["sensor_status"]["external_ds18b20"] = "read_error"

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    validated_names = {item["measurement"] for item in validated}

    assert validated_names == {
        "internal_temperature",
        "internal_humidity",
        "water_temperature",
    }

    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "external_temperature"
    assert rejected[0]["sensor_id"] == "external_ds18b20"
    assert rejected[0]["reason_code"] == "sensor_read_error"
    assert rejected[0]["received_value"] is None

def test_device_id_mismatch_rejects_entire_message():
    payload = deepcopy(VALID_PAYLOAD)
    payload["device_id"] = "different-device"

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["rejection_scope"] == "message"
    assert rejected[0]["device_id"] == "esp32s3-01"
    assert rejected[0]["reason_code"] == "device_id_mismatch"


def test_unsupported_schema_rejects_entire_message():
    payload = deepcopy(VALID_PAYLOAD)
    payload["schema_version"] = 2

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["reason_code"] == "unsupported_schema"


def test_message_level_rejection_preserves_boot_id_and_sequence_when_present():
    payload = deepcopy(VALID_PAYLOAD)
    payload["schema_version"] = 2

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert rejected[0]["boot_id"] == VALID_PAYLOAD["boot_id"]
    assert rejected[0]["sequence"] == VALID_PAYLOAD["sequence"]


def test_missing_metadata_rejects_entire_message():
    payload = deepcopy(VALID_PAYLOAD)
    del payload["boot_id"]

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["reason_code"] == "missing_metadata"


def test_negative_sequence_rejects_entire_message():
    payload = deepcopy(VALID_PAYLOAD)
    payload["sequence"] = -1

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["reason_code"] == "invalid_metadata"

def test_missing_measurement_rejects_only_that_measurement():
    payload = deepcopy(VALID_PAYLOAD)
    del payload["measurements"]["water_temperature_c"]

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "water_temperature"
    assert rejected[0]["reason_code"] == "missing_measurement"


def test_out_of_range_measurement_is_rejected():
    payload = deepcopy(VALID_PAYLOAD)
    payload["measurements"]["water_temperature_c"] = 85.0

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "water_temperature"
    assert rejected[0]["reason_code"] == "out_of_plausible_range"


def test_boolean_measurement_is_rejected_as_invalid_type():
    payload = deepcopy(VALID_PAYLOAD)
    payload["measurements"]["internal_humidity_percent"] = True

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "internal_humidity"
    assert rejected[0]["reason_code"] == "invalid_type"

def test_empty_raw_payload_is_rejected():
    validated, rejected = validate_raw_payload(
        b"",
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["reason_code"] == "empty_payload"


def test_invalid_json_is_rejected():
    validated, rejected = validate_raw_payload(
        b'{"device_id":',
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert validated == []
    assert len(rejected) == 1
    assert rejected[0]["reason_code"] == "invalid_json"


def test_valid_json_bytes_are_validated():
    import json

    raw_payload = json.dumps(VALID_PAYLOAD).encode("utf-8")

    validated, rejected = validate_raw_payload(
        raw_payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 4
    assert rejected == []

def test_missing_sensor_status_rejects_only_affected_measurement():
    payload = deepcopy(VALID_PAYLOAD)
    del payload["sensor_status"]["external_ds18b20"]

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "external_temperature"
    assert rejected[0]["reason_code"] == "missing_sensor_status"


def test_unknown_sensor_status_rejects_only_affected_measurement():
    payload = deepcopy(VALID_PAYLOAD)
    payload["sensor_status"]["external_ds18b20"] = "warming_up"

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "external_temperature"
    assert rejected[0]["reason_code"] == "invalid_sensor_status"

def test_null_value_with_ok_status_rejects_only_that_measurement():
    payload = deepcopy(VALID_PAYLOAD)
    payload["measurements"]["internal_temperature_c"] = None
    # internal_sht31 remains "ok"

    validated, rejected = validate_raw_message(
        payload,
        topic_device_id="esp32s3-01",
        timestamp=TIMESTAMP,
    )

    assert len(validated) == 3
    assert len(rejected) == 1
    assert rejected[0]["measurement"] == "internal_temperature"
    assert rejected[0]["reason_code"] == "invalid_type"
    assert rejected[0]["received_value"] is None
