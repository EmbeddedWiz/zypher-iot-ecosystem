/**
 * @file gpio_manager.hpp
 * @brief Hardware Abstraction Layer for ESP32-S3 peripherals.
 *
 * Manages standard digital GPIOs and addressable WS2812 RGB LEDs.
 * Implements interrupt-driven button handling with safe memory relocation.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"

/** @brief GPIO Pin for the BOOT button (hardware fixed) */
#define ZYPHER_BUTTON_GPIO  GPIO_NUM_0

/**
 * @brief Global Interrupt Service Routine for Button events.
 *
 * @note Marked extern "C" and IRAM_ATTR to ensure static relocation
 * and placement in high-speed memory for GCC 14 compatibility.
 *
 * @param arg Pointer to the boolean flag to be updated.
 */
extern "C" void IRAM_ATTR zypher_button_isr_handler(void* arg) {
    if (arg != nullptr) {
        // Direct pointer cast to avoid complex class member access inside ISR
        volatile bool* const flag = reinterpret_cast<volatile bool*>(arg);
        // Active LOW button logic (0 = Pressed)
        *flag = (gpio_get_level(ZYPHER_BUTTON_GPIO) == 0);
    }
}

/**
 * @class GpioManager
 * @brief Singleton driver for local hardware resources.
 */
class GpioManager {
public:
    /** @brief Access the Singleton instance */
    static GpioManager& get_instance() {
        static GpioManager instance;
        return instance;
    }

    /**
     * @brief Initialize all GPIO and RMT subsystems.
     * Sets up digital outputs, button interrupts, and WS2812 driver.
     */
    void initialize() {
        ESP_LOGI("GPIO", "Initializing hardware subsystems...");

        // 1. Digital Outputs Configuration (Status and External RGB)
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << STATUS_LED_GPIO) | 
                               (1ULL << RGB_RED_GPIO) | 
                               (1ULL << RGB_GREEN_GPIO) | 
                               (1ULL << RGB_BLUE_GPIO);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        (void)gpio_config(&io_conf);

        // 2. BOOT Button Configuration with Edge-Triggered Interrupts
        gpio_config_t btn_conf = {};
        btn_conf.intr_type = GPIO_INTR_ANYEDGE;
        btn_conf.mode = GPIO_MODE_INPUT;
        btn_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
        btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        (void)gpio_config(&btn_conf);

        // Register hardware ISR handler
        (void)gpio_install_isr_service(0);
        (void)gpio_isr_handler_add(BUTTON_GPIO, zypher_button_isr_handler, static_cast<void*>(const_cast<bool*>(&m_button_pressed)));

        // Read initial physical state
        m_button_pressed = (gpio_get_level(BUTTON_GPIO) == 0);

        // 3. WS2812 Addressable LED strip initialization (RMT backend)
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = RGB_STRIP_GPIO;
        strip_config.max_leds = 1U;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10U * 1000U * 1000U; // 10MHz timing precision
        rmt_config.flags.with_dma = false;

        const esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
        if (err == ESP_OK) {
            is_strip_initialized = true;
            (void)led_strip_clear(led_strip);
            ESP_LOGI("GPIO", "On-board WS2812 driver ready on GPIO48.");
        }

        // Set default safe state
        set_rgb_led(0U, 0U, 0U);
        set_simple_led(false);
    }

    /**
     * @brief Set the color of the WS2812 LED and mirror to digital pins.
     *
     * @note Optimized to call the blocking RMT refresh only if values changed.
     *
     * @param r Red intensity (0-255)
     * @param g Green intensity (0-255)
     * @param b Blue intensity (0-255)
     */
    void set_rgb_led(uint8_t r, uint8_t g, uint8_t b) {
        // Delta check to minimize RMT overhead
        if ((m_last_r == r) && (m_last_g == g) && (m_last_b == b)) {
            return;
        }

        m_last_r = r; m_last_g = g; m_last_b = b;

        if (is_strip_initialized && (led_strip != nullptr)) {
            if ((r == 0U) && (g == 0U) && (b == 0U)) {
                (void)led_strip_clear(led_strip);
            } else {
                (void)led_strip_set_pixel(led_strip, 0U, r, g, b);
                (void)led_strip_refresh(led_strip);
            }
        }

        // PWM simulation via simple digital threshold
        gpio_set_level(RGB_RED_GPIO, (r >= 128U) ? 1 : 0);
        gpio_set_level(RGB_GREEN_GPIO, (g >= 128U) ? 1 : 0);
        gpio_set_level(RGB_BLUE_GPIO, (b >= 128U) ? 1 : 0);
    }

    /** @brief Direct control for the single-color status LED */
    void set_simple_led(bool state) {
        (void)gpio_set_level(STATUS_LED_GPIO, state ? 1 : 0);
    }

    /** @brief Alias for status LED control */
    void set_status_led(bool state) {
        set_simple_led(state);
    }

    /** @brief Get the atomic button state from ISR */
    bool get_button_state() const {
        return m_button_pressed;
    }

private:
    GpioManager() = default;

    // Pin definitions
    static constexpr gpio_num_t RGB_STRIP_GPIO  = GPIO_NUM_48;
    static constexpr gpio_num_t STATUS_LED_GPIO = GPIO_NUM_2;
    static constexpr gpio_num_t RGB_RED_GPIO    = GPIO_NUM_4;
    static constexpr gpio_num_t RGB_GREEN_GPIO  = GPIO_NUM_5;
    static constexpr gpio_num_t RGB_BLUE_GPIO   = GPIO_NUM_6;
    static constexpr gpio_num_t BUTTON_GPIO     = GPIO_NUM_0;

    led_strip_handle_t led_strip = nullptr;
    bool is_strip_initialized = false;

    /** @brief Button state flag, modified by ISR */
    volatile bool m_button_pressed{false};

    // Cached values
    uint8_t m_last_r{0U}, m_last_g{0U}, m_last_b{0U};
};
