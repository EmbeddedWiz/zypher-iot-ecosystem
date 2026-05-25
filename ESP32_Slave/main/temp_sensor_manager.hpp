/**
 * @file temp_sensor_manager.hpp
 * @brief Driver for ESP32 internal CPU temperature sensor.
 */

#pragma once

#include "driver/temperature_sensor.h"
#include "esp_log.h"

/**
 * @class TempSensorManager
 * @brief Singleton for CPU temperature monitoring.
 */
class TempSensorManager {
public:
    /** @brief Access the Singleton instance */
    static TempSensorManager& get_instance() {
        static TempSensorManager instance;
        return instance;
    }

    /**
     * @brief Configure and start the hardware sensor.
     */
    void initialize() {
        ESP_LOGI("TEMP", "Initializing internal sensor...");

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        esp_err_t err = temperature_sensor_install(&temp_sensor_config, &temp_handle);

        if (err == ESP_OK) {
            ESP_ERROR_CHECK(temperature_sensor_enable(temp_handle));
            is_initialized = true;
        } else {
            ESP_LOGE("TEMP", "Failed to install temperature sensor!");
        }
    }

    /**
     * @brief Read the current die temperature in Celsius.
     * @return Temperature float or 0.0 if failed.
     */
    float read_temperature() {
        if (!is_initialized) return 0.0f;

        float tsens_value;
        if (temperature_sensor_get_celsius(temp_handle, &tsens_value) == ESP_OK) {
            return tsens_value;
        }
        return 0.0f;
    }

private:
    TempSensorManager() = default;

    temperature_sensor_handle_t temp_handle = nullptr;
    bool is_initialized = false;
};
