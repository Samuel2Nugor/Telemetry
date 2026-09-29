#include "mqtt_publisher.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

extern const char mqtt_ca_crt_start[]
    asm("_binary_mqtt_ca_crt_start");

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_CONNECT_TIMEOUT_MS 30000

static const char *TAG = "mqtt_publisher";

static const char *RAW_TELEMETRY_TOPIC =
    "telemetry/v1/devices/"
    CONFIG_TELEMETRY_DEVICE_ID
    "/telemetry/raw";

static EventGroupHandle_t s_mqtt_event_group;
static esp_mqtt_client_handle_t s_mqtt_client;
static bool s_started;

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_args;
    (void)event_base;

    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "QoS 1 message acknowledged, msg_id=%d",
                 event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT client reported an error");

        if (event->error_handle != NULL) {
            ESP_LOGE(
                TAG,
                "TLS error=0x%x, transport error=0x%x, socket errno=%d",
                event->error_handle->esp_tls_last_esp_err,
                event->error_handle->esp_tls_stack_err,
                event->error_handle->esp_transport_sock_errno
            );
        }
        break;

    default:
        break;
    }
}

esp_err_t mqtt_publisher_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "MQTT publisher is already started");
        return ESP_OK;
    }

    if (strlen(CONFIG_TELEMETRY_MQTT_BROKER_URI) == 0 ||
        strlen(CONFIG_TELEMETRY_MQTT_USERNAME) == 0 ||
        strlen(CONFIG_TELEMETRY_MQTT_PASSWORD) == 0) {
        ESP_LOGE(TAG, "MQTT configuration is incomplete");
        return ESP_ERR_INVALID_STATE;
    }

    if (strncmp(CONFIG_TELEMETRY_MQTT_BROKER_URI,
                "mqtts://", 8) != 0) {
        ESP_LOGE(TAG, "MQTT broker URI must use mqtts://");
        return ESP_ERR_INVALID_ARG;
    }

    s_mqtt_event_group = xEventGroupCreate();

    if (s_mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Could not create MQTT event group");
        return ESP_ERR_NO_MEM;
    }

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri =
            CONFIG_TELEMETRY_MQTT_BROKER_URI,

        .broker.verification.certificate = mqtt_ca_crt_start,
        .broker.verification.skip_cert_common_name_check = false,

        .credentials.client_id = CONFIG_TELEMETRY_DEVICE_ID,
        .credentials.username =
            CONFIG_TELEMETRY_MQTT_USERNAME,

        .credentials.authentication.password =
            CONFIG_TELEMETRY_MQTT_PASSWORD,

        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);

    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Could not initialize MQTT client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_mqtt_client_register_event(
        s_mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register MQTT event handler: %s",
                 esp_err_to_name(error));
        return error;
    }

    error = esp_mqtt_client_start(s_mqtt_client);

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start MQTT client: %s",
                 esp_err_to_name(error));
        return error;
    }

    s_started = true;

    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MQTT_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS)
    );

    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for MQTT connection");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t mqtt_publisher_publish_raw(const char *payload)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_started || s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT publisher has not been started");
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);

    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Cannot publish while MQTT is disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    int message_id = esp_mqtt_client_publish(
        s_mqtt_client,
        RAW_TELEMETRY_TOPIC,
        payload,
        0,
        1,
        0
    );

    if (message_id < 0) {
        ESP_LOGE(TAG, "Could not queue MQTT message");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Raw telemetry queued, msg_id=%d", message_id);
    return ESP_OK;
}
