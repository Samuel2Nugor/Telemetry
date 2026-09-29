#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "ds18b20_manager.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_publisher.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sht31.h"
#include "wifi_manager.h"

static const char *TAG = "telemetry";

static void initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();

    if (
        result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_LOGW(TAG, "Erasing incompatible NVS data");
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }

    ESP_ERROR_CHECK(result);
}

static void format_temperature_value(
    float temperature_c,
    bool valid,
    char *buffer,
    size_t buffer_size
)
{
    if (valid) {
        snprintf(buffer, buffer_size, "%.2f", temperature_c);
    } else {
        snprintf(buffer, buffer_size, "null");
    }
}

static esp_err_t publish_telemetry(
    const char *boot_id,
    uint32_t sequence,
    float internal_temperature_c,
    float internal_humidity_percent,
    const ds18b20_readings_t *ds18b20_readings
)
{
    const int64_t uptime_ms = esp_timer_get_time() / 1000;

    char payload[512];
    char water_temperature_value[24];
    char external_temperature_value[24];

    format_temperature_value(
        ds18b20_readings->water_temperature_c,
        ds18b20_readings->water_valid,
        water_temperature_value,
        sizeof(water_temperature_value)
    );

    format_temperature_value(
        ds18b20_readings->external_temperature_c,
        ds18b20_readings->external_valid,
        external_temperature_value,
        sizeof(external_temperature_value)
    );

    const int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{"
            "\"schema_version\":1,"
            "\"device_id\":\"%s\","
            "\"boot_id\":\"%s\","
            "\"sequence\":%" PRIu32 ","
            "\"uptime_ms\":%" PRIi64 ","
            "\"measurements\":{"
                "\"internal_temperature_c\":%.2f,"
                "\"internal_humidity_percent\":%.2f,"
                "\"external_temperature_c\":%s,"
                "\"water_temperature_c\":%s"
            "},"
            "\"sensor_status\":{"
                "\"internal_sht31\":\"ok\","
                "\"external_ds18b20\":\"%s\","
                "\"water_ds18b20\":\"%s\""
            "}"
        "}",
        CONFIG_TELEMETRY_DEVICE_ID,
        boot_id,
        sequence,
        uptime_ms,
        internal_temperature_c,
        internal_humidity_percent,
        external_temperature_value,
        water_temperature_value,
        ds18b20_readings->external_status,
        ds18b20_readings->water_status
    );

    if (
        payload_length < 0 ||
        (size_t)payload_length >= sizeof(payload)
    ) {
        ESP_LOGE(
            TAG,
            "Telemetry payload buffer is too small"
        );
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(
        TAG,
        "Publishing telemetry: boot_id=%s, sequence=%" PRIu32,
        boot_id,
        sequence
    );

    ESP_LOGI(
        TAG,
        "Internal SHT31 telemetry: temperature=%.2f C humidity=%.2f %%",
        internal_temperature_c,
        internal_humidity_percent
    );

    return mqtt_publisher_publish_raw(payload);
}

static esp_err_t synchronize_time(void)
{
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    esp_err_t result = esp_netif_sntp_init(&config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SNTP initialization failed: %s",
            esp_err_to_name(result)
        );
        return result;
    }

    ESP_LOGI(TAG, "Waiting for network time");

    result = esp_netif_sntp_sync_wait(
        pdMS_TO_TICKS(60000)
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Time synchronization failed: %s",
            esp_err_to_name(result)
        );
        esp_netif_sntp_deinit();
        return result;
    }

    time_t now;
    struct tm utc_time;
    char time_text[32];

    time(&now);
    gmtime_r(&now, &utc_time);

    strftime(
        time_text,
        sizeof(time_text),
        "%Y-%m-%dT%H:%M:%SZ",
        &utc_time
    );

    ESP_LOGI(
        TAG,
        "Time synchronized: %s",
        time_text
    );

    return ESP_OK;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const size_t psram_total_bytes =
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    const size_t psram_free_bytes =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(
        TAG,
        "Telemetry firmware started"
    );

    ESP_LOGI(
        TAG,
        "CPU cores: %u, chip revision: %u",
        (unsigned int)chip_info.cores,
        (unsigned int)chip_info.revision
    );

    ESP_LOGI(
        TAG,
        "PSRAM heap: total=%zu bytes, free=%zu bytes",
        psram_total_bytes,
        psram_free_bytes
    );

    initialize_nvs();

    const esp_err_t sht31_result = sht31_init();

    if (sht31_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SHT31 initialization failed: %s",
            esp_err_to_name(sht31_result)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "SHT31 initialization succeeded"
    );

    const esp_err_t ds18b20_init_result = ds18b20_manager_init();

    if (ds18b20_init_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DS18B20 initialization failed: %s",
            esp_err_to_name(ds18b20_init_result)
        );
    }

    const esp_err_t wifi_result =
        wifi_manager_connect();

    if (wifi_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Wi-Fi initialization failed: %s",
            esp_err_to_name(wifi_result)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi connection is ready"
    );

    const esp_err_t time_result =
        synchronize_time();

    if (time_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "MQTT startup stopped: time is not synchronized"
        );
        return;
    }

    const esp_err_t mqtt_result =
        mqtt_publisher_start();

    if (mqtt_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "MQTT initialization failed: %s",
            esp_err_to_name(mqtt_result)
        );
        return;
    }

    char boot_id[9];
    snprintf(
        boot_id,
        sizeof(boot_id),
        "%08" PRIx32,
        esp_random()
    );

    uint32_t sequence = 0;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        float internal_temperature_c;
        float internal_humidity_percent;

        const esp_err_t read_result = sht31_read(
            &internal_temperature_c,
            &internal_humidity_percent
        );

        if (read_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "SHT31 read failed: %s",
                esp_err_to_name(read_result)
            );
            vTaskDelayUntil(
                &last_wake_time,
                pdMS_TO_TICKS(CONFIG_TELEMETRY_INTERVAL_MS)
            );
            continue;
        }

        ds18b20_readings_t ds18b20_readings;
        const esp_err_t ds18b20_read_result =
            ds18b20_manager_read(&ds18b20_readings);

        if (ds18b20_read_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "DS18B20 read failed: %s",
                esp_err_to_name(ds18b20_read_result)
            );
        }

        const esp_err_t publish_result = publish_telemetry(
            boot_id,
            sequence,
            internal_temperature_c,
            internal_humidity_percent,
            &ds18b20_readings
        );

        if (publish_result == ESP_OK) {
            ESP_LOGI(TAG, "Telemetry was queued successfully");
        } else {
            ESP_LOGE(
                TAG,
                "Telemetry publication failed: %s",
                esp_err_to_name(publish_result)
            );
        }

        /*
         * Advance unconditionally: a failed publish must still consume its
         * sequence number, otherwise a later successful publish would reuse
         * a sequence the backend already saw and get dropped as a duplicate,
         * silently losing real data instead of the sample that failed.
         */
        sequence++;

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(CONFIG_TELEMETRY_INTERVAL_MS)
        );
    }
}
