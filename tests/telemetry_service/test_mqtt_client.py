import logging
from types import SimpleNamespace
from unittest.mock import Mock

import telemetry_service.mqtt_client as mqtt_client

import pytest

from telemetry_service.mqtt_client import (
    build_rejected_topic,
    build_validated_topic,
    extract_device_id,
    TelemetryMqttService,
)


def test_extract_device_id_from_raw_topic():
    topic = "telemetry/v1/devices/esp32s3-01/telemetry/raw"

    assert extract_device_id(topic) == "esp32s3-01"


@pytest.mark.parametrize(
    "topic",
    [
        "",
        "telemetry/v1/devices//telemetry/raw",
        "telemetry/v1/devices/esp32s3-01/telemetry",
        "telemetry/v1/devices/esp32s3-01/status",
        "another/v1/devices/esp32s3-01/telemetry/raw",
    ],
)
def test_invalid_raw_topic_returns_none(topic):
    assert extract_device_id(topic) is None


def test_build_validated_topic():
    topic = build_validated_topic(
        device_id="esp32s3-01",
        measurement="water_temperature",
    )

    assert topic == (
        "telemetry/v1/devices/esp32s3-01/"
        "telemetry/validated/water_temperature"
    )


def test_build_rejected_topic():
    topic = build_rejected_topic(device_id="esp32s3-01")

    assert topic == (
        "telemetry/v1/devices/esp32s3-01/telemetry/rejected"
    )

def test_on_message_logs_processing_summary_and_publishes(monkeypatch, caplog):
    validated = [
        {
            "measurement": "internal_temperature",
            "value": 23.6,
        }
    ]
    rejected = [
        {
            "measurement": "water_temperature",
            "reason_code": "sensor_read_error",
        }
    ]

    monkeypatch.setattr(
        mqtt_client,
        "validate_raw_payload",
        Mock(return_value=(validated, rejected)),
    )

    service = object.__new__(TelemetryMqttService)
    service._recent_message_keys = mqtt_client.OrderedDict()
    publish_json = Mock(return_value=True)
    monkeypatch.setattr(service, "_publish_json", publish_json)

    message = SimpleNamespace(
        topic="telemetry/v1/devices/esp32s3-01/telemetry/raw",
        payload=b'{"test":true}',
    )

    with caplog.at_level(logging.INFO, logger=mqtt_client.__name__):
        service._on_message(None, None, message)

    assert "Received raw telemetry: device_id=esp32s3-01" in caplog.text
    assert (
        "Validation completed: device_id=esp32s3-01 "
        "validated=1 rejected=1"
    ) in caplog.text
    assert publish_json.call_count == 2

def test_duplicate_message_is_published_only_once(monkeypatch):
    validated = [{
        "device_id": "esp32s3-01",
        "boot_id": "boot-1",
        "sequence": 42,
        "measurement": "internal_temperature",
        "value": 23.6,
    }]
    monkeypatch.setattr(
        mqtt_client,
        "validate_raw_payload",
        Mock(return_value=(validated, [])),
    )

    service = object.__new__(TelemetryMqttService)
    service._recent_message_keys = mqtt_client.OrderedDict()
    publish_json = Mock(return_value=True)
    monkeypatch.setattr(service, "_publish_json", publish_json)

    message = SimpleNamespace(
        topic="telemetry/v1/devices/esp32s3-01/telemetry/raw",
        payload=b'{"device_id":"esp32s3-01","boot_id":"boot-1","sequence":42}',
    )

    service._on_message(None, None, message)
    service._on_message(None, None, message)

    assert publish_json.call_count == 1

def test_new_sequence_or_boot_is_published(monkeypatch):
    def validate_message(raw_payload, *, topic_device_id, timestamp):
        raw = mqtt_client.json.loads(raw_payload)
        return ([{
            "device_id": topic_device_id,
            "boot_id": raw["boot_id"],
            "sequence": raw["sequence"],
            "measurement": "internal_temperature",
            "value": 23.6,
        }], [])

    monkeypatch.setattr(mqtt_client, "validate_raw_payload", validate_message)

    service = object.__new__(TelemetryMqttService)
    service._recent_message_keys = mqtt_client.OrderedDict()
    publish_json = Mock(return_value=True)
    monkeypatch.setattr(service, "_publish_json", publish_json)

    topic = "telemetry/v1/devices/esp32s3-01/telemetry/raw"
    for boot_id, sequence in [
        ("boot-1", 42),
        ("boot-1", 43),
        ("boot-2", 42),
        ("boot-1", 42),  # Duplicate of the first message
    ]:
        message = SimpleNamespace(
            topic=topic,
            payload=mqtt_client.json.dumps({
                "boot_id": boot_id,
                "sequence": sequence,
            }).encode("utf-8"),
        )
        service._on_message(None, None, message)

    assert publish_json.call_count == 3
