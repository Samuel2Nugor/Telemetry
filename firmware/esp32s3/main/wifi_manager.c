#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRY_COUNT 5

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;
static esp_event_handler_instance_t s_wifi_event_handler;
static esp_event_handler_instance_t s_ip_event_handler;

/*
 * esp_wifi_connect() legitimately returns non-OK for transient driver
 * states (e.g. a connection attempt already in progress). ESP_ERROR_CHECK
 * on that would abort and reboot the device on ordinary Wi-Fi flakiness, so
 * failures here are logged and left to the next disconnect event to retry.
 */
static void request_reconnect(void)
{
    const esp_err_t result = esp_wifi_connect();

    if (result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "esp_wifi_connect() call failed: %s",
            esp_err_to_name(result)
        );
    }
}

static void handle_wifi_event(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_argument;

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    ) {
        request_reconnect();
        return;
    }

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {
        if (s_retry_count < WIFI_MAXIMUM_RETRY_COUNT) {
            s_retry_count++;
            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected; retrying (%d/%d)",
                s_retry_count,
                WIFI_MAXIMUM_RETRY_COUNT
            );
            request_reconnect();
        } else {
            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );
        }

        return;
    }

    if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {
        const ip_event_got_ip_t *event =
            (const ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Connected with IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        s_retry_count = 0;
        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

esp_err_t wifi_manager_connect(void)
{
    if (strlen(CONFIG_TELEMETRY_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Wi-Fi SSID is not configured");
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi station interface");
        return ESP_FAIL;
    }

    const wifi_init_config_t initialization =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&initialization));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &handle_wifi_event,
            NULL,
            &s_wifi_event_handler
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &handle_wifi_event,
            NULL,
            &s_ip_event_handler
        )
    );

    wifi_config_t configuration = {0};

    if (
        strlcpy(
            (char *)configuration.sta.ssid,
            CONFIG_TELEMETRY_WIFI_SSID,
            sizeof(configuration.sta.ssid)
        ) >= sizeof(configuration.sta.ssid)
    ) {
        ESP_LOGE(TAG, "Wi-Fi SSID is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    if (
        strlcpy(
            (char *)configuration.sta.password,
            CONFIG_TELEMETRY_WIFI_PASSWORD,
            sizeof(configuration.sta.password)
        ) >= sizeof(configuration.sta.password)
    ) {
        ESP_LOGE(TAG, "Wi-Fi password is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &configuration
        )
    );
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(
        TAG,
        "Connecting to configured Wi-Fi network"
    );

    const EventBits_t result = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if ((result & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "Could not connect after %d retries",
        WIFI_MAXIMUM_RETRY_COUNT
    );

    return ESP_FAIL;
}
