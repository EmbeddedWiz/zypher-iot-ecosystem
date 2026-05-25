/**
 * @file main.cpp
 * @brief Application entry point for the Zypher ESP32-S3 Slave.
 *
 * Coordinates subsystem initialization and hands off control
 * to the multi-threaded network engine.
 *
 * @author Zypher Project Team
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "gpio_manager.hpp"
#include "temp_sensor_manager.hpp"
#include "network_processor.hpp"

/**
 * @brief System main entry point.
 * Runs on Core 0 after bootloader initialization.
 */
extern "C" void app_main() {
    ESP_LOGI("SYSTEM", "--- Booting Zypher ESP32-S3 Instance ---");

    /**
     * @section NVS
     * Initialize Non-Volatile Storage (NVS) required for Wi-Fi stack
     * and potential configuration storage.
     */
    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /**
     * @section DRIVERS
     * Initialize hardware abstraction layers.
     */
    GpioManager::get_instance().initialize();
    TempSensorManager::get_instance().initialize();

    /**
     * @section TASKS
     * Launch the network processor state machine.
     * This call returns immediately while tasks run in background.
     */
    NetworkProcessor::get_instance().start_tasks();

    ESP_LOGI("SYSTEM", "Initialization completed. System stable.");
}
