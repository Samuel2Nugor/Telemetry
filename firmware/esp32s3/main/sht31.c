#include "sht31.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"



#define SHT31_ADDRESS_PRIMARY   0x44
#define SHT31_ADDRESS_SECONDARY 0x45
#define SHT31_SDA_GPIO CONFIG_TELEMETRY_SHT31_SDA_GPIO
#define SHT31_SCL_GPIO CONFIG_TELEMETRY_SHT31_SCL_GPIO

#define SHT31_I2C_FREQUENCY_HZ 100000

static const char *TAG = "sht31";

static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t device_handle = NULL;

static uint8_t sht31_crc8(
    const uint8_t *data,
    size_t length
)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];

        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

esp_err_t sht31_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = SHT31_SCL_GPIO,
        .sda_io_num = SHT31_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t result = i2c_new_master_bus(
        &bus_config,
        &bus_handle
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "I2C bus initialization failed: %s",
            esp_err_to_name(result)
        );
        return result;
    }

    ESP_LOGI(
        TAG,
        "I2C initialized: SDA=GPIO%d SCL=GPIO%d",
        SHT31_SDA_GPIO,
        SHT31_SCL_GPIO
    );

    uint8_t detected_address = SHT31_ADDRESS_PRIMARY;

    result = i2c_master_probe(
        bus_handle,
        detected_address,
        1000
    );

    if (result != ESP_OK) {
        detected_address = SHT31_ADDRESS_SECONDARY;

        result = i2c_master_probe(
            bus_handle,
            detected_address,
            1000
        );
    }

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SHT31 not detected at 0x44 or 0x45"
        );
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(
        TAG,
        "SHT31 detected at address 0x%02X",
        detected_address
    );

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = detected_address,
        .scl_speed_hz = SHT31_I2C_FREQUENCY_HZ,
    };

    result = i2c_master_bus_add_device(
        bus_handle,
        &device_config,
        &device_handle
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register SHT31 device: %s",
            esp_err_to_name(result)
        );
        return result;
    }

    ESP_LOGI(TAG, "SHT31 device registered");

    return ESP_OK;
}

esp_err_t sht31_read(
    float *temperature_c,
    float *humidity_percent
)
{
    if (
        device_handle == NULL ||
        temperature_c == NULL ||
        humidity_percent == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t command[2] = {
        0x24,
        0x00
    };

    esp_err_t result = i2c_master_transmit(
        device_handle,
        command,
        sizeof(command),
        1000
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SHT31 measurement command failed: %s",
            esp_err_to_name(result)
        );
        return result;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];

    result = i2c_master_receive(
        device_handle,
        data,
        sizeof(data),
        1000
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SHT31 measurement read failed: %s",
            esp_err_to_name(result)
        );
        return result;
    }

    const uint8_t temperature_crc =
        sht31_crc8(data, 2);

    const uint8_t humidity_crc =
        sht31_crc8(&data[3], 2);

    if (
        temperature_crc != data[2] ||
        humidity_crc != data[5]
    ) {
        ESP_LOGE(
            TAG,
            "SHT31 CRC validation failed"
        );

        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature =
        ((uint16_t)data[0] << 8) |
        data[1];

    const uint16_t raw_humidity =
        ((uint16_t)data[3] << 8) |
        data[4];

    *temperature_c =
        -45.0f +
        175.0f *
        ((float)raw_temperature / 65535.0f);

    *humidity_percent =
        100.0f *
        ((float)raw_humidity / 65535.0f);

    ESP_LOGI(
        TAG,
        "SHT31 reading: temperature=%.2f C humidity=%.2f %%",
        *temperature_c,
        *humidity_percent
    );

    return ESP_OK;
}
