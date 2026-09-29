import json
import logging
from collections import OrderedDict
from datetime import datetime, timezone
from typing import Any
import ssl
import paho.mqtt.client as mqtt

from telemetry_service.config import Settings
from telemetry_service.validator import validate_raw_payload

LOGGER = logging.getLogger(__name__)

BASE_TOPIC = "telemetry/v1/devices"
RAW_TOPIC_FILTER = f"{BASE_TOPIC}/+/telemetry/raw"
MQTT_QOS = 1
# Deduplication is now per measurement rather than per raw message (up to
# four keys per telemetry cycle), so the cap is raised to keep roughly the
# same wall-clock dedup window.
MAX_RECENT_MESSAGES = 16384


class TelemetryMqttService:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self._recent_message_keys = OrderedDict()

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=settings.mqtt_client_id,
            protocol=mqtt.MQTTv311,
        )

        self.client.username_pw_set(
            username=settings.mqtt_username,
            password=settings.mqtt_password,
        )

        tls_context = ssl.create_default_context(
            cafile=settings.mqtt_ca_cert,
        )
        tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.client.tls_set_context(tls_context)

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def run(self) -> None:
        LOGGER.info(
            "Connecting to MQTT broker at %s:%s",
            self.settings.mqtt_host,
            self.settings.mqtt_port,
        )

        self.client.connect_async(
            host=self.settings.mqtt_host,
            port=self.settings.mqtt_port,
            keepalive=self.settings.mqtt_keepalive_seconds,
        )

        self.client.loop_forever(retry_first_connection=True)

    def _on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties,
    ) -> None:
        if reason_code.is_failure:
            LOGGER.error("MQTT connection failed: %s", reason_code)
            return

        LOGGER.info("Connected to MQTT broker")

        result, _ = client.subscribe(
            RAW_TOPIC_FILTER,
            qos=MQTT_QOS,
        )

        if result != mqtt.MQTT_ERR_SUCCESS:
            LOGGER.error(
                "Failed to subscribe to %s: result %s",
                RAW_TOPIC_FILTER,
                result,
            )
            return

        LOGGER.info("Subscribed to %s", RAW_TOPIC_FILTER)

    def _on_message(self, client, userdata, message) -> None:
        device_id = extract_device_id(message.topic)

        if device_id is None:
            LOGGER.warning(
                "Ignoring message with unexpected topic: %s",
                message.topic,
            )
            return

        timestamp = current_utc_timestamp()

        LOGGER.info(
            "Received raw telemetry: device_id=%s topic=%s bytes=%d",
            device_id,
            message.topic,
            len(message.payload),
        )

        validated, rejected = validate_raw_payload(
            message.payload,
            topic_device_id=device_id,
            timestamp=timestamp,
        )

        LOGGER.info(
            "Validation completed: device_id=%s validated=%d rejected=%d",
            device_id,
            len(validated),
            len(rejected),
        )

        for payload in validated:
            topic = build_validated_topic(
                device_id=device_id,
                measurement=payload["measurement"],
            )
            self._publish_deduplicated(
                device_id=device_id,
                payload=payload,
                topic=topic,
                dedup_suffix=payload["measurement"],
            )

        rejected_topic = build_rejected_topic(device_id=device_id)

        for payload in rejected:
            self._publish_deduplicated(
                device_id=device_id,
                payload=payload,
                topic=rejected_topic,
                dedup_suffix=payload.get("measurement", "message"),
            )

    def _publish_deduplicated(
        self,
        *,
        device_id: str,
        payload: dict[str, Any],
        topic: str,
        dedup_suffix: str,
    ) -> None:
        # Deduplicate per measurement (not per raw MQTT message): if only
        # some of a message's measurements published successfully before a
        # QoS 1 redelivery, only the ones that actually failed get retried
        # instead of republishing everything and double-counting in Influx.
        key = _dedup_key(
            device_id=device_id,
            payload=payload,
            suffix=dedup_suffix,
        )

        if key is not None and key in self._recent_message_keys:
            LOGGER.info("Dropping duplicate telemetry: key=%s", key)
            return

        if not self._publish_json(topic=topic, payload=payload):
            return

        if key is not None:
            self._recent_message_keys[key] = None
            if len(self._recent_message_keys) > MAX_RECENT_MESSAGES:
                self._recent_message_keys.popitem(last=False)

    def _publish_json(
        self,
        *,
        topic: str,
        payload: dict[str, Any],
    ) -> bool:
        encoded_payload = json.dumps(
            payload,
            allow_nan=False,
            separators=(",", ":"),
        )

        result = self.client.publish(
            topic=topic,
            payload=encoded_payload,
            qos=MQTT_QOS,
            retain=False,
        )

        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            LOGGER.error(
                "Failed to publish to %s: result %s",
                topic,
                result.rc,
            )
            return False

        LOGGER.info(
            "Queued MQTT publish: topic=%s qos=%d",
            topic,
            MQTT_QOS,
        )
        return True




def _dedup_key(
    *,
    device_id: str,
    payload: dict[str, Any],
    suffix: str,
) -> tuple[str, str, int, str] | None:
    boot_id = payload.get("boot_id")
    sequence = payload.get("sequence")

    if (
        not isinstance(boot_id, str)
        or not boot_id
        or not isinstance(sequence, int)
        or isinstance(sequence, bool)
    ):
        return None

    return (device_id, boot_id, sequence, suffix)


def current_utc_timestamp() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z")
    )


def extract_device_id(topic: str) -> str | None:
    parts = topic.split("/")

    if len(parts) != 6:
        return None

    if parts[0:3] != ["telemetry", "v1", "devices"]:
        return None

    if parts[4:6] != ["telemetry", "raw"]:
        return None

    device_id = parts[3]

    if not device_id:
        return None

    return device_id


def build_validated_topic(
    *,
    device_id: str,
    measurement: str,
) -> str:
    _validate_topic_segment(device_id, "device_id")
    _validate_topic_segment(measurement, "measurement")

    return (
        f"{BASE_TOPIC}/{device_id}/"
        f"telemetry/validated/{measurement}"
    )


def build_rejected_topic(*, device_id: str) -> str:
    _validate_topic_segment(device_id, "device_id")

    return f"{BASE_TOPIC}/{device_id}/telemetry/rejected"


def _validate_topic_segment(value: str, name: str) -> None:
    if (
        not isinstance(value, str)
        or not value
        or "/" in value
        or "+" in value
        or "#" in value
    ):
        raise ValueError(f"Invalid MQTT topic segment: {name}")