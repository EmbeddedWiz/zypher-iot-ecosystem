/**
 * @file network_processor.hpp
 * @brief High-performance network engine for Zypher ESP32-S3 Slave.
 *
 * DESIGN PRINCIPLES:
 * 1. Determinism: Tasks are pinned to specific cores to avoid scheduling jitter.
 * 2. MISRA C++:2023 Compliance: Zero dynamic memory allocation, strict type
 *    safety, and explicit casting.
 * 3. Observability: Integrated CPU load monitoring and thermal reporting.
 *
 * ARCHITECTURE (SMP):
 * - Core 0: Managed LwIP stack, Wi-Fi connectivity, and Protocol RX/TX.
 * - Core 1: Real-time hardware feedback and diagnostic patterns.
 */

#pragma once

#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <fcntl.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"

#include "protocol.hpp"
#include "wifi_credentials.hpp"
#include "gpio_manager.hpp"
#include "temp_sensor_manager.hpp"
#include "wifi_manager.hpp"

/**
 * @namespace NetConfig
 * @brief Functional constants for the Zypher protocol tuning.
 * Using 'constexpr' instead of '#define' for strict type checking (MISRA Rule).
 */
namespace NetConfig {
    /** @brief Max peak brightness for wait-state breathing effect */
    constexpr float    MAX_BREATH_BRIGHTNESS = 40.0f;
    /** @brief Communication watchdog timeout (ms) */
    constexpr uint32_t MASTER_TIMEOUT_MS     = 30000U;
    /** @brief Frequency of discovery announcements (ms) */
    constexpr uint32_t DISCOVERY_INTERVAL_MS = 3000U;
    /** @brief Frequency of telemetry datagrams (ms) */
    constexpr uint32_t TELEMETRY_INTERVAL_MS = 2000U;
    /** @brief Standard UDP buffer size for IoT payloads */
    constexpr uint16_t RX_BUFFER_SIZE        = 512U;
    /** @brief Refresh rate for visual effects (33 FPS) */
    constexpr uint32_t BREATH_STEP_MS        = 30U;
    /** @brief Cycle duration (attack/decay) for breathing effect */
    constexpr uint32_t BREATH_HALF_PERIOD_MS = 3000U;
}

/**
 * @enum DeviceState
 * @brief Discrete operational states for the FSM (Finite State Machine).
 */
enum class DeviceState : uint8_t {
    SEARCHING_WIFI,      /**< Initial state: Waiting for DHCP lease */
    WAITING_FOR_MASTER,  /**< Network ready: Broadcasting discovery packets */
    MASTER_CONTROL       /**< Authenticated: Exclusive session with Android node */
};

/**
 * @class NetworkProcessor
 * @brief Singleton engine coordinating communication and hardware synchronization.
 */
class NetworkProcessor {
public:
    /** @brief Provides thread-safe access to the static instance */
    static NetworkProcessor& get_instance() {
        static NetworkProcessor instance;
        return instance;
    }

    /** @brief Get the current system state (Atomic load) */
    DeviceState get_state() const {
        return m_state.load();
    }

    /**
     * @brief Parallel task deployment.
     * Segregates networking and hardware to different CPU cores to ensure
     * that network heavy processing doesn't degrade visual performance.
     */
    void start_tasks() {
        (void)xTaskCreatePinnedToCore(net_task_entry, "net_task", 8192, this, 5, nullptr, 0);
        (void)xTaskCreatePinnedToCore(hardware_task_entry, "hardware_task", 4096, this, 5, nullptr, 1);
    }

private:
    NetworkProcessor() = default;

    /** @brief Entry point for the networking logic (Core 0) */
    static void net_task_entry(void* pvParameters) {
        static_cast<NetworkProcessor*>(pvParameters)->run_net_processor();
    }

    /** @brief Entry point for hardware patterns (Core 1) */
    static void hardware_task_entry(void* pvParameters) {
        static_cast<NetworkProcessor*>(pvParameters)->run_hardware_diagnostics();
    }

    /**
     * @brief Robust string copying.
     * Adheres to safety standards by preventing buffer overflows and
     * ensuring proper null-termination.
     */
    void safe_strncpy(char* dest, const char* src, size_t n) {
        if (n > 0U) {
            size_t i = 0;
            // copy up to N-1 characters or until null terminator
            while (i + 1 < n && src[i] != '\0') {
                dest[i] = src[i];
                ++i;
            }
            dest[i] = '\0';
        }
    }

    /**
     * @brief Core 0: Network Processor.
     * Manages the life cycle of the UDP control channel.
     */
    void run_net_processor() {
        ESP_LOGI("NET_CORE", "Initiating Zypher Network Engine...");

        // Initialize Wi-Fi driver with credentials from common/
        WifiManager::get_instance().initialize(
            wifi_credentials[0].ssid, wifi_credentials[0].pass,
            wifi_credentials[1].ssid, wifi_credentials[1].pass);

        // Open non-blocking UDP socket
        const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE("NET_CORE", "Critical failure: Socket creation");
            return;
        }

        int broadcast_opt = 1;
        (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));

        const int flags = fcntl(sock, F_GETFL, 0);
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in listen_addr = {};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(ZypherProtocol::BROADCAST_PORT);

        if (bind(sock, reinterpret_cast<const struct sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
            ESP_LOGE("NET_CORE", "Critical failure: Socket bind");
            (void)close(sock);
            return;
        }

        uint64_t last_discovery_sent = 0U;
        uint64_t last_telemetry_sent = 0U;
        uint8_t rx_buffer[NetConfig::RX_BUFFER_SIZE];
        bool last_reported_button_state = false;

        while (true) {
            const uint64_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
            const bool wifi_connected = WifiManager::get_instance().is_connected();
            const DeviceState current_state = m_state.load();

            // 1. Connection Monitoring (FSM transitions)
            if (!wifi_connected) {
                if (current_state != DeviceState::SEARCHING_WIFI) {
                    m_state.store(DeviceState::SEARCHING_WIFI);
                    m_session_token = 0U;
                    ESP_LOGW("NET_CORE", "Link lost. System reset to Search.");
                }
            } else if (current_state == DeviceState::SEARCHING_WIFI) {
                m_state.store(DeviceState::WAITING_FOR_MASTER);
                ESP_LOGI("NET_CORE", "Link ready. System online.");
            } else { /* Stable state maintained */ }

            // 2. Watchdog: Disconnect master if silent for >30s
            if (m_state.load() == DeviceState::MASTER_CONTROL) {
                if ((now_ms - m_last_master_activity.load()) >= NetConfig::MASTER_TIMEOUT_MS) {
                    ESP_LOGW("NET_CORE", "Watchdog: Master communication timeout.");
                    m_state.store(DeviceState::WAITING_FOR_MASTER);
                    m_session_token = 0U;
                }
            }

            // 3. Outbound Transmitter (Discovery/Telemetry)
            if (m_state.load() == DeviceState::WAITING_FOR_MASTER) {
                if ((now_ms - last_discovery_sent) >= NetConfig::DISCOVERY_INTERVAL_MS) {
                    last_discovery_sent = now_ms;
                    // Rotate challenge using Hardware TRNG
                    m_current_challenge = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();

                    ZypherProtocol::DiscoveryPacket pkt = {};
                    safe_strncpy(pkt.board_name, "Zypher-S3-Slave", 16U);
                    safe_strncpy(pkt.service_class, "LED_SYSTEM", 16U);
                    safe_strncpy(pkt.serial_number, "SN-ESP541-01", 16U);

                    uint8_t mac[6];
                    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
                    (void)snprintf(pkt.mac_address, sizeof(pkt.mac_address), "%02X:%02X:%02X:%02X:%02X:%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

                    pkt.challenge_nonce = m_current_challenge;

                    struct sockaddr_in dest_addr = {};
                    dest_addr.sin_family = AF_INET;
                    dest_addr.sin_port = htons(ZypherProtocol::BROADCAST_PORT);
                    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                    (void)sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<const struct sockaddr*>(&dest_addr), sizeof(dest_addr));
                }
            } else if (m_state.load() == DeviceState::MASTER_CONTROL) {
                const bool btn = GpioManager::get_instance().get_button_state();
                // Send telemetry periodically or immediately on hardware interrupt event
                if (((now_ms - last_telemetry_sent) >= NetConfig::TELEMETRY_INTERVAL_MS) || (btn != last_reported_button_state)) {
                    last_telemetry_sent = now_ms;
                    last_reported_button_state = btn;

                    ZypherProtocol::TelemetryPacket pkt = {};
                    pkt.uptime_seconds = static_cast<uint32_t>(now_ms / 1000U);
                    pkt.button_state = btn ? 1U : 0U;
                    pkt.cpu_temperature = TempSensorManager::get_instance().read_temperature();
                    // Load calculation (Simulated for this demo)
                    pkt.core_0_load = 15U;
                    pkt.core_1_load = 5U;

                    (void)sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<const struct sockaddr*>(&m_master_addr), sizeof(m_master_addr));
                }
            } else { /* No packets sent in SEARCHING_WIFI mode */ }

            // 4. Inbound Receiver (Datagram processing)
            struct sockaddr_in src_addr = {};
            socklen_t addr_len = sizeof(src_addr);
            const int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

            if (len > 0) {
                const uint8_t type = rx_buffer[0];

                // --- CASE 1: CRYPTOGRAPHIC CAPTURE ---
                if (type == static_cast<uint8_t>(ZypherProtocol::PacketType::MASTER_CAPTURE)) {
                    if (static_cast<size_t>(len) >= sizeof(ZypherProtocol::CapturePacket)) {
                        auto* cap = reinterpret_cast<ZypherProtocol::CapturePacket*>(rx_buffer);
                        const uint64_t expected = ZypherProtocol::compute_response(m_current_challenge, ZypherProtocol::PRE_SHARED_SECRET);

                        if (cap->challenge_response == expected) {
                            m_state.store(DeviceState::MASTER_CONTROL);
                            m_session_token = cap->challenge_response;
                            m_master_addr = src_addr;
                            safe_strncpy(m_locked_master_id, cap->master_id, 16U);
                            m_last_master_activity.store(now_ms);

                            // Send positive handshake ACK
                            uint8_t ack_pkt[1] = { static_cast<uint8_t>(ZypherProtocol::PacketType::CAPTURE_ACK) };
                            (void)sendto(sock, ack_pkt, 1, 0, reinterpret_cast<const struct sockaddr*>(&src_addr), sizeof(src_addr));

                            // Restore physical UI state
                            GpioManager::get_instance().set_rgb_led(m_saved_r, m_saved_g, m_saved_b);
                            ESP_LOGI("NET_CORE", "Exclusive control granted to '%s'.", m_locked_master_id);
                        }
                    }
                }
                // --- CASE 2: REAL-TIME ACTUATOR CONTROL ---
                else if (type == static_cast<uint8_t>(ZypherProtocol::PacketType::MASTER_CONTROL)) {
                    if (static_cast<size_t>(len) >= sizeof(ZypherProtocol::ControlPacket)) {
                        auto* ctrl = reinterpret_cast<ZypherProtocol::ControlPacket*>(rx_buffer);
                        // Auth verification for every single control packet
                        if ((ctrl->session_token == m_session_token) && (m_state.load() == DeviceState::MASTER_CONTROL)) {
                            m_last_master_activity.store(now_ms);

                            m_saved_r = ctrl->led_r;
                            m_saved_g = ctrl->led_g;
                            m_saved_b = ctrl->led_b;

                            GpioManager::get_instance().set_status_led(ctrl->status_led == 1U);
                            GpioManager::get_instance().set_rgb_led(m_saved_r, m_saved_g, m_saved_b);
                        }
                    }
                }
                // --- CASE 3: SESSION TERMINATION ---
                else if (type == static_cast<uint8_t>(ZypherProtocol::PacketType::MASTER_RELEASE)) {
                    uint8_t ack_pkt[1] = { static_cast<uint8_t>(ZypherProtocol::PacketType::RELEASE_ACK) };
                    (void)sendto(sock, ack_pkt, 1, 0, reinterpret_cast<const struct sockaddr*>(&src_addr), sizeof(src_addr));

                    m_state.store(DeviceState::WAITING_FOR_MASTER);
                    m_session_token = 0U;
                    ESP_LOGI("NET_CORE", "Session closed by Master request.");
                } else { /* Illegal packet discarded */ }
            }

            vTaskDelay(pdMS_TO_TICKS(10U));
        }
    }

    /**
     * @brief Core 1: Hardware UI Task.
     * Manages visual feedback patterns without interrupting network traffic.
     */
    void run_hardware_diagnostics() {
        ESP_LOGI("CTRL_CORE", "Diagnostic engine online.");
        uint32_t breath_tick = 0U;

        while (true) {
            const DeviceState state = m_state.load();

            if (state == DeviceState::SEARCHING_WIFI) {
                // Heartbeat pulse: 70ms ON, ~3s OFF
                GpioManager::get_instance().set_status_led(true);
                GpioManager::get_instance().set_rgb_led(0U, 0U, 100U);
                vTaskDelay(pdMS_TO_TICKS(70U));

                GpioManager::get_instance().set_status_led(false);
                GpioManager::get_instance().set_rgb_led(0U, 0U, 0U);
                vTaskDelay(pdMS_TO_TICKS(2930U));
            }
            else if (state == DeviceState::WAITING_FOR_MASTER) {
                // Breathing logic: Triangle wave mapped to LED intensity
                const uint32_t steps_in_half = NetConfig::BREATH_HALF_PERIOD_MS / NetConfig::BREATH_STEP_MS;

                const float progress = static_cast<float>(breath_tick % steps_in_half) / static_cast<float>(steps_in_half);
                const bool fading_out = ((breath_tick / steps_in_half) % 2U) != 0U;

                const float alpha = fading_out ? (1.0f - progress) : progress;
                const uint8_t val = static_cast<uint8_t>(alpha * NetConfig::MAX_BREATH_BRIGHTNESS);

                GpioManager::get_instance().set_rgb_led(0U, 0U, val);

                breath_tick++;
                vTaskDelay(pdMS_TO_TICKS(NetConfig::BREATH_STEP_MS));
            }
            else {
                // Suspended mode during active Control
                breath_tick = 0U;
                vTaskDelay(pdMS_TO_TICKS(200U));
            }
        }
    }

    /** @brief Shared atomic system state */
    std::atomic<DeviceState> m_state{DeviceState::SEARCHING_WIFI};
    /** @brief Destination address of the last authorized Master */
    struct sockaddr_in m_master_addr{};
    /** @brief Plaintext identifier of the session owner */
    char m_locked_master_id[16]{};
    /** @brief Active session token (Challenge-Response result) */
    uint64_t m_session_token = 0U;
    /** @brief Valid challenge nonce for the current window */
    uint64_t m_current_challenge = 0U;
    /** @brief Last activity timestamp for watchdog monitoring */
    std::atomic<uint64_t> m_last_master_activity{0U};

    /** @brief Cached Red channel value for session recovery */
    uint8_t m_saved_r{0U};
    /** @brief Cached Green channel value for session recovery */
    uint8_t m_saved_g{0U};
    /** @brief Cached Blue channel value for session recovery */
    uint8_t m_saved_b{0U};
};
