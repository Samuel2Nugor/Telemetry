import pytest

from telemetry_service.config import ConfigurationError, load_settings


VALID_ENVIRONMENT = {
    "MQTT_HOST": "mosquitto",
    "MQTT_PORT": "1883",
    "MQTT_USERNAME": "telemetry-service",
    "MQTT_PASSWORD": "test-password",
    "MQTT_CLIENT_ID": "telemetry-service",
    "MQTT_KEEPALIVE_SECONDS": "60",
    "MQTT_CA_CERT": "/etc/telemetry/certs/ca.crt",
}


def set_valid_environment(monkeypatch):
    for name, value in VALID_ENVIRONMENT.items():
        monkeypatch.setenv(name, value)


def test_load_settings_reads_valid_environment(monkeypatch):
    set_valid_environment(monkeypatch)

    settings = load_settings()

    assert settings.mqtt_host == "mosquitto"
    assert settings.mqtt_port == 1883
    assert settings.mqtt_username == "telemetry-service"
    assert settings.mqtt_password == "test-password"
    assert settings.mqtt_client_id == "telemetry-service"
    assert settings.mqtt_keepalive_seconds == 60
    assert settings.mqtt_ca_cert == "/etc/telemetry/certs/ca.crt"


def test_missing_password_raises_configuration_error(monkeypatch):
    set_valid_environment(monkeypatch)
    monkeypatch.delenv("MQTT_PASSWORD")

    with pytest.raises(ConfigurationError, match="MQTT_PASSWORD"):
        load_settings()


def test_invalid_port_raises_configuration_error(monkeypatch):
    set_valid_environment(monkeypatch)
    monkeypatch.setenv("MQTT_PORT", "not-a-number")

    with pytest.raises(ConfigurationError, match="MQTT_PORT"):
        load_settings()