/**
 * @file wifi_manager.hpp
 * @brief Singleton Wi-Fi connectivity driver.
 *
 * Implements a fail-safe connection logic that cycles through primary
 * and secondary credentials until a stable IP address is allocated.
 */

#pragma once

#include <cstring>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"

/**
 * @class WifiManager
 * @brief Singleton responsible for managing the Wi‑Fi station lifecycle on the ESP32‑S3.
 *
 * This class encapsulates all Wi‑Fi related operations: initializing the driver,
 * configuring SSID credentials, handling connection retries, and rotating through a
 * list of fallback access points when the current one fails. It is implemented as a
 * thread‑safe singleton so that any part of the firmware can obtain a reference via
 * `WifiManager::get_instance()` without worrying about duplicate initialisation.
 *
 * The design mirrors a typical state‑machine: start → attempt connection → on
 * failure retry a few times → switch to the next SSID → repeat. All logging is done
 * through ESP‑IDF's `ESP_LOG` facilities to aid debugging on the serial monitor.
 */
class WifiManager {
public:
    /** @brief Access the Singleton instance */
    static WifiManager& get_instance() {
        static WifiManager instance;
        return instance;
    }

    /**
     * @brief Initialize Wi-Fi and start the connection attempt cycle.
     *
     * @param ssid1 Primary Network Name
     * @param pass1 Primary Password
     * @param ssid2 Secondary Fallback Network Name
     * @param pass2 Secondary Fallback Password
     */
    void initialize(const char* ssid1, const char* pass1,
                    const char* ssid2 = nullptr, const char* pass2 = nullptr) {

    // Store the primary (index 0) and optional secondary (index 1) credentials.
    // The manager works with a fixed‑size array because the number of fallback
    // networks is small in this project. `m_num_creds` keeps track of how many
    // entries are actually valid – either 1 or 2.
    // NOTE: The function now accepts raw C‑strings; they are not copied because the
    // caller guarantees that the strings outlive the Wi‑Fi manager (they come from
    // the global `wifi_credentials` table).
    m_creds[0] = {ssid1, pass1};
    m_creds[1] = {ssid2, pass2};
    m_num_creds = (ssid2 != nullptr && pass2 != nullptr) ? 2U : 1U;

        s_wifi_event_group = xEventGroupCreate();

        // Init lower-level TCP/IP stack
        (void)esp_netif_init();
        (void)esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        (void)esp_wifi_init(&cfg);

        // Register handlers for connection and IP events
        esp_event_handler_instance_t any_id, got_ip_id;
        (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this, &any_id);
        (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, this, &got_ip_id);

            // Set the Wi‑Fi interface to station mode.
            (void)esp_wifi_set_mode(WIFI_MODE_STA);
            // Auto‑connect is intentionally disabled because the manager
            // performs manual SSID rotation. The ESP‑IDF function
            // `esp_wifi_set_auto_connect` is unavailable in the current SDK version,
            // so the line is left commented for documentation purposes.
            // (void)esp_wifi_set_auto_connect(false); // Disabled auto‑connect (function not available in this ESP‑IDF version)


        start_connection_attempt();
    }

    /** @brief Check if the device currently has a valid IP and Wi-Fi link */
    bool is_connected() const {
        return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0U;
    }

private:
    WifiManager() = default;

    /** @brief Private structure for SSID/Password pair */
    struct Cred { const char* ssid; const char* pass; };

    Cred     m_creds[2];
    uint32_t m_num_creds = 0U;
    uint32_t m_current_index = 0U;
    uint32_t m_retry_count = 0U;

    /** @brief Number of retries before switching to next SSID */
    static constexpr uint32_t MAX_RETRIES_PER_CONFIG = 3U;

    /**
     * @brief Configure the radio and initiate physical layer connection.
     */
    void start_connection_attempt() {
        if (m_num_creds == 0U) { return; }

        wifi_config_t wifi_config = {};
        const Cred& current = m_creds[m_current_index];

        // Prepare the Wi‑Fi configuration structure with the selected
        // credential. We use `std::strncpy` to ensure the SSID and password are
        // safely copied into the fixed‑size ESP‑IDF buffers.
        (void)std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), current.ssid, sizeof(wifi_config.sta.ssid));
        (void)std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), current.pass, sizeof(wifi_config.sta.password));

        // Minimal RSSI threshold – we accept any signal strength.
        wifi_config.sta.threshold.rssi = -127;

        ESP_LOGI("WIFI_SYSTEM", "Connecting to %s (Attempt %lu/%lu)", current.ssid, m_retry_count + 1U, MAX_RETRIES_PER_CONFIG);

        // Force a clean disconnect before applying the new configuration. This
        // prevents the driver from holding onto a previous AP's state.
        (void)esp_wifi_disconnect();
        (void)esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

        static bool started = false;
        if (!started) {
            (void)esp_wifi_start();
            started = true;
        } else {
            (void)esp_wifi_connect();
        }
    }

    /**
     * @brief ESP-IDF Event loop callback.
     * Handles disconnection events and switches SSID indices.
     */
    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
        WifiManager* const self = static_cast<WifiManager*>(arg);

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            (void)esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            (void)xEventGroupClearBits(self->s_wifi_event_group, WIFI_CONNECTED_BIT);

            self->m_retry_count++;
            if (self->m_retry_count >= MAX_RETRIES_PER_CONFIG) {
                // Switch to secondary SSID if primary failed
                // We have exceeded the configured retry limit for the current SSID.
                // Reset the retry counter, move to the next credential in the
                // circular list, and log the transition for debugging.
                self->m_retry_count = 0U;
                self->m_current_index = (self->m_current_index + 1U) % self->m_num_creds;
                ESP_LOGW("WIFI_SYSTEM", "Switching to next SSID index (now using SSID \"%s\").",
                             self->m_creds[self->m_current_index].ssid);
                // Small back‑off gives the radio time to settle before the next
                // connect attempt.
                vTaskDelay(pdMS_TO_TICKS(200));
                self->start_connection_attempt();
            } else {
                (void)esp_wifi_connect();
            }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            // A successful IP acquisition indicates that the device is now
            // connected to the selected AP. We clear any retry counters and set
            // the event‑group bit so `is_connected()` returns true.
            self->m_retry_count = 0U;
            (void)xEventGroupSetBits(self->s_wifi_event_group, WIFI_CONNECTED_BIT);
            // Cast event_data to the proper type
            // Cast event_data to the proper type for IP_EVENT_STA_GOT_IP
            ip_event_got_ip_t* ip_event = static_cast<ip_event_got_ip_t*>(event_data);
            ESP_LOGI("WIFI_SYSTEM", "Link ready! IP: " IPSTR,
                     IP2STR(&ip_event->ip_info.ip));
        } else { /* Unhandled */ }
    }

    EventGroupHandle_t s_wifi_event_group = nullptr;
    static constexpr uint32_t WIFI_CONNECTED_BIT = BIT0;
};
