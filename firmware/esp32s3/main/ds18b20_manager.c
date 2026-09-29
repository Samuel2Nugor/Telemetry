#include "ds18b20_manager.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "ds18b20.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "onewire_bus.h"
#include "onewire_device.h"
#include "sdkconfig.h"

static const char *TAG = "ds18b20_manager";

#define WATER_DS18B20_ROM \
    ((uint64_t)CONFIG_TELEMETRY_WATER_DS18B20_ROM)

#define EXTERNAL_DS18B20_ROM \
    ((uint64_t)CONFIG_TELEMETRY_EXTERNAL_DS18B20_ROM)

#define ONEWIRE_GPIO \
    ((gpio_num_t)CONFIG_TELEMETRY_ONEWIRE_GPIO)

#define ONEWIRE_UART_PORT \
    CONFIG_TELEMETRY_ONEWIRE_UART_PORT

/*
 * The DS18B20 returns this exact value (raw register 0x0550, an exact binary
 * fraction) after a power-on reset or a parasitic-power brownout on the bus.
 * A successful read still reports ESP_OK, so it must be filtered here or it
 * reaches the backend labelled "ok" and looks like a real 85 C spike.
 */
#define DS18B20_POWER_ON_RESET_SENTINEL_C 85.0f

static bool is_power_on_reset_sentinel(float temperature_c)
{
    return temperature_c == DS18B20_POWER_ON_RESET_SENTINEL_C;
}

static onewire_bus_handle_t s_bus = NULL;
static ds18b20_device_handle_t s_water_device = NULL;
static ds18b20_device_handle_t s_external_device = NULL;
static bool s_initialized = false;

/*
 * On the current ESP32-S3 and ESP-IDF 6.0.1 setup, the UART backend's first
 * ROM search returns no devices unless the bus receives this reset first.
 */
static void prime_onewire_bus(void)
{
    gpio_reset_pin(ONEWIRE_GPIO);
    gpio_set_direction(ONEWIRE_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(ONEWIRE_GPIO, 1);
    esp_rom_delay_us(1000);

    gpio_set_level(ONEWIRE_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(ONEWIRE_GPIO, 1);
    esp_rom_delay_us(480);

    gpio_reset_pin(ONEWIRE_GPIO);
}

static esp_err_t register_device(
    onewire_device_t *discovered_device,
    ds18b20_device_handle_t *device
)
{
    ds18b20_config_t config = {};

    return ds18b20_new_device_from_enumeration(
        discovered_device,
        &config,
        device
    );
}

esp_err_t ds18b20_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    prime_onewire_bus();

    onewire_bus_config_t bus_config = {
        .bus_gpio_num = ONEWIRE_GPIO,
        .flags = {
            .en_pull_up = true,
        },
    };

    onewire_bus_uart_config_t uart_config = {
        .uart_port_num = ONEWIRE_UART_PORT,
    };

    esp_err_t result = onewire_new_bus_uart(
        &bus_config,
        &uart_config,
        &s_bus
    );

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "1-Wire bus setup failed: %s", esp_err_to_name(result));
        return result;
    }

    onewire_device_iter_handle_t iter = NULL;
    result = onewire_new_device_iter(s_bus, &iter);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "1-Wire iterator failed: %s", esp_err_to_name(result));
        onewire_bus_del(s_bus);
        s_bus = NULL;
        return result;
    }

    int device_count = 0;
    onewire_device_t discovered_device;

    while (
        (result = onewire_device_iter_get_next(
            iter,
            &discovered_device
        )) == ESP_OK
    ) {
        const uint64_t rom = (uint64_t)discovered_device.address;
        ds18b20_device_handle_t *device = NULL;

        ESP_LOGI(TAG, "1-Wire ROM: %016" PRIX64, rom);

        if (rom == WATER_DS18B20_ROM) {
            device = &s_water_device;
        } else if (rom == EXTERNAL_DS18B20_ROM) {
            device = &s_external_device;
        } else {
            ESP_LOGW(TAG, "Unknown 1-Wire device ignored: %016" PRIX64, rom);
        }

        if (device != NULL) {
            const esp_err_t device_result = register_device(
                &discovered_device,
                device
            );

            if (device_result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "DS18B20 registration failed for ROM %016" PRIX64 ": %s",
                    rom,
                    esp_err_to_name(device_result)
                );
            }
        }

        device_count++;
    }

    onewire_del_device_iter(iter);

    if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "1-Wire scan failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "1-Wire scan complete: %d device(s)", device_count);

    if (s_water_device == NULL && s_external_device == NULL) {
        ESP_LOGE(TAG, "No configured DS18B20 device found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(
        TAG,
        "DS18B20 roles: water=%s external=%s",
        s_water_device != NULL ? "ready" : "missing",
        s_external_device != NULL ? "ready" : "missing"
    );

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ds18b20_manager_read(ds18b20_readings_t *readings)
{
    if (readings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *readings = (ds18b20_readings_t) {
        .water_status = "not_detected",
        .external_status = "not_detected",
    };

    if (!s_initialized || s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_water_device != NULL) {
        readings->water_status = "read_error";
    }

    if (s_external_device != NULL) {
        readings->external_status = "read_error";
    }

    esp_err_t result = ds18b20_trigger_temperature_conversion_for_all(s_bus);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 conversion failed: %s", esp_err_to_name(result));
        return result;
    }

    if (s_water_device != NULL) {
        result = ds18b20_get_temperature(
            s_water_device,
            &readings->water_temperature_c
        );

        if (result == ESP_OK && is_power_on_reset_sentinel(readings->water_temperature_c)) {
            readings->water_status = "invalid_value";
            ESP_LOGW(
                TAG,
                "Water DS18B20 reported the power-on-reset sentinel "
                "(85.00 C); treating the reading as invalid"
            );
        } else if (result == ESP_OK) {
            readings->water_valid = true;
            readings->water_status = "ok";
            ESP_LOGI(
                TAG,
                "Water DS18B20 (%016" PRIX64 ") temperature: %.2f C",
                WATER_DS18B20_ROM,
                readings->water_temperature_c
            );
        } else {
            ESP_LOGE(TAG, "Water DS18B20 read failed: %s", esp_err_to_name(result));
        }
    }

    if (s_external_device != NULL) {
        result = ds18b20_get_temperature(
            s_external_device,
            &readings->external_temperature_c
        );

        if (result == ESP_OK && is_power_on_reset_sentinel(readings->external_temperature_c)) {
            readings->external_status = "invalid_value";
            ESP_LOGW(
                TAG,
                "External DS18B20 reported the power-on-reset sentinel "
                "(85.00 C); treating the reading as invalid"
            );
        } else if (result == ESP_OK) {
            readings->external_valid = true;
            readings->external_status = "ok";
            ESP_LOGI(
                TAG,
                "External DS18B20 (%016" PRIX64 ") temperature: %.2f C",
                EXTERNAL_DS18B20_ROM,
                readings->external_temperature_c
            );
        } else {
            ESP_LOGE(
                TAG,
                "External DS18B20 read failed: %s",
                esp_err_to_name(result)
            );
        }
    }

    if (!readings->water_valid && !readings->external_valid) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
