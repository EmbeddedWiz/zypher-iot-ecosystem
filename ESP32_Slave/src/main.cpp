#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/rand32.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>

#include <protocol.hpp>

#include <string>
#include <cstring>
#include <atomic>
#include <cstdlib>

LOG_MODULE_REGISTER(zypher_station, LOG_LEVEL_INF);

// --- Wi-Fi Credentials Configuration ---
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPASS"

// --- Thread Stack and Configuration ---
#define THREAD_STACK_SIZE 4096
#define THREAD_PRIORITY 7

K_THREAD_STACK_DEFINE(net_thread_stack, THREAD_STACK_SIZE);
struct k_thread net_thread_data;

K_THREAD_STACK_DEFINE(ctrl_thread_stack, THREAD_STACK_SIZE);
struct k_thread ctrl_thread_data;

// --- Device State and Authentication variables ---
enum class DeviceState {
    UNINITIALIZED,
    CONTROLLED
};

std::atomic<DeviceState> g_device_state{DeviceState::UNINITIALIZED};
struct sockaddr_in g_master_addr{};
char g_locked_master_id[16]{};
uint64_t g_session_token = 0;
uint64_t g_current_challenge = 0;
std::atomic<int64_t> g_last_master_activity{0};

// --- GpioManager: Direct Low-level Control of Pins ---
class GpioManager {
public:
    static GpioManager& get_instance() {
        static GpioManager instance;
        return instance;
    }

    bool initialize() {
        // Fetch standard GPIO controller (usually node "gpio0" on ESP32)
        gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
        if (!device_is_ready(gpio_dev)) {
            printk("Error: GPIO controller device not ready!\n");
            return false;
        }

        // Configure Status LED (Built-in LED, usually pin 2)
        int ret = gpio_pin_configure(gpio_dev, STATUS_LED_PIN, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) return false;

        // Configure RGB Pins (R: 4, G: 5, B: 6)
        ret = gpio_pin_configure(gpio_dev, RGB_RED_PIN, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) return false;
        ret = gpio_pin_configure(gpio_dev, RGB_GREEN_PIN, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) return false;
        ret = gpio_pin_configure(gpio_dev, RGB_BLUE_PIN, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) return false;

        // Configure Boot Button (Pin 0, active LOW, with pull-up)
        ret = gpio_pin_configure(gpio_dev, BUTTON_PIN, GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) return false;

        return true;
    }

    void set_status_led(bool state) {
        if (gpio_dev) {
            gpio_pin_set(gpio_dev, STATUS_LED_PIN, state ? 1 : 0);
        }
    }

    void set_rgb_led(uint8_t r, uint8_t g, uint8_t b) {
        if (gpio_dev) {
            // On a digital output, treat >= 128 as HIGH (ON) and < 128 as LOW (OFF)
            gpio_pin_set(gpio_dev, RGB_RED_PIN, r >= 128 ? 1 : 0);
            gpio_pin_set(gpio_dev, RGB_GREEN_PIN, g >= 128 ? 1 : 0);
            gpio_pin_set(gpio_dev, RGB_BLUE_PIN, b >= 128 ? 1 : 0);
        }
    }

    bool get_button_state() {
        if (gpio_dev) {
            // Button is active LOW (0 means pressed, 1 means released)
            return gpio_pin_get(gpio_dev, BUTTON_PIN) == 0;
        }
        return false;
    }

private:
    GpioManager() = default;
    const struct device* gpio_dev = nullptr;

    static constexpr gpio_pin_t STATUS_LED_PIN = 2;
    static constexpr gpio_pin_t RGB_RED_PIN    = 4;
    static constexpr gpio_pin_t RGB_GREEN_PIN  = 5;
    static constexpr gpio_pin_t RGB_BLUE_PIN   = 6;
    static constexpr gpio_pin_t BUTTON_PIN     = 0;
};

// --- Wi-Fi Connection logic ---
void connect_to_wifi() {
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No default network interface found!");
        return;
    }

    struct wifi_connect_req_params params = {};
    params.ssid = reinterpret_cast<const uint8_t*>(WIFI_SSID);
    params.ssid_length = std::strlen(WIFI_SSID);
    params.psk = reinterpret_cast<const uint8_t*>(WIFI_PASSWORD);
    params.psk_length = std::strlen(WIFI_PASSWORD);
    params.channel = WIFI_CHANNEL_ANY;
    params.security = WIFI_SECURITY_TYPE_PSK; // WPA2-PSK

    LOG_INF("Connecting to Wi-Fi SSID: '%s'...", WIFI_SSID);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(struct wifi_connect_req_params));
    if (ret != 0) {
        LOG_ERR("Wi-Fi connection request failed: %d", ret);
    }
}

// --- Thread [Core 0]: Network & UDP Communication ---
void net_thread_entry(void *p1, void *p2, void *p3) {
    LOG_INF("[Core 0] Network thread started and successfully pinned to Core 0!");

    // Start Wi-Fi connection procedure
    connect_to_wifi();

    // Give some time for Wi-Fi association and DHCP to resolve IP
    k_sleep(K_MSEC(5000));

    // Create UDP Socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        LOG_ERR("[Core 0] Failed to create UDP socket: errno %d", errno);
        return;
    }

    // Set SO_BROADCAST option
    int broadcast_opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt)) < 0) {
        LOG_ERR("[Core 0] Failed to enable SO_BROADCAST: errno %d", errno);
        close(sock);
        return;
    }

    // Set non-blocking socket mode
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Bind to BROADCAST_PORT
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(ZypherProtocol::BROADCAST_PORT);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        LOG_ERR("[Core 0] Failed to bind UDP socket: errno %d", errno);
        close(sock);
        return;
    }

    LOG_INF("[Core 0] UDP Socket successfully bound and listening on port %d", ZypherProtocol::BROADCAST_PORT);

    uint64_t last_discovery_sent = 0;
    uint64_t last_telemetry_sent = 0;
    uint8_t rx_buffer[256];

    while (true) {
        uint64_t now_ms = k_uptime_get();

        // 1. Connection Timeout Check (1 minute / 60 seconds)
        if (g_device_state == DeviceState::CONTROLLED) {
            int64_t inactive_duration = now_ms - g_last_master_activity.load();
            if (inactive_duration >= 60000) {
                LOG_WRN("[Core 0] Keep-alive timeout! No master packets for %lld ms.", inactive_duration);
                LOG_WRN("[Core 0] Reverting to UNINITIALIZED state...");
                
                g_device_state = DeviceState::UNINITIALIZED;
                g_session_token = 0;
                std::memset(g_locked_master_id, 0, sizeof(g_locked_master_id));
                
                // Reset outputs
                GpioManager::get_instance().set_rgb_led(0, 0, 0);
            }
        }

        // 2. State-specific background transmitters
        if (g_device_state == DeviceState::UNINITIALIZED) {
            // Broadcaster: Every 3 seconds
            if (now_ms - last_discovery_sent >= 3000) {
                last_discovery_sent = now_ms;

                // Roll a 64-bit random challenge nonce
                g_current_challenge = sys_rand32_get();
                g_current_challenge = (g_current_challenge << 32) | sys_rand32_get();

                ZypherProtocol::DiscoveryPacket pkt{};
                std::strncpy(pkt.board_name, "Zypher-ESP32S3", sizeof(pkt.board_name));
                std::strncpy(pkt.service_class, "LED_CONTROLLER", sizeof(pkt.service_class));
                std::strncpy(pkt.serial_number, "SN-ZEPHYR-99", sizeof(pkt.serial_number));
                pkt.challenge_nonce = g_current_challenge;

                struct sockaddr_in dest_addr{};
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(ZypherProtocol::BROADCAST_PORT);
                dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // Broadcast IP

                int ret = sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
                if (ret < 0) {
                    LOG_DBG("[Core 0] Send broadcast failed: %d", errno);
                } else {
                    LOG_INF("[Core 0] Sent Discovery Broadcast (Challenge Nonce = 0x%llX)", g_current_challenge);
                }
            }
        } else if (g_device_state == DeviceState::CONTROLLED) {
            // Telemetry: Every 2 seconds
            if (now_ms - last_telemetry_sent >= 2000) {
                last_telemetry_sent = now_ms;

                ZypherProtocol::TelemetryPacket pkt{};
                pkt.uptime_seconds = static_cast<uint32_t>(now_ms / 1000);
                pkt.button_state = GpioManager::get_instance().get_button_state() ? 1 : 0;
                
                // Simulate CPU thermal changes under Core 0 & Core 1 relative thread loads
                float baseline_temp = 36.8f;
                float core_fluctuation = 0.03f * (std::rand() % 100);
                pkt.cpu_temperature = baseline_temp + core_fluctuation;

                // Load calculations for cores
                pkt.core_0_load = 18 + (std::rand() % 8);  // active networking & sockets
                pkt.core_1_load = 4 + (std::rand() % 4);   // control loop

                int ret = sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr*>(&g_master_addr), sizeof(g_master_addr));
                if (ret < 0) {
                    LOG_ERR("[Core 0] Telemetry transmission failed: %d", errno);
                } else {
                    LOG_DBG("[Core 0] Telemetry sent successfully.");
                }
            }
        }

        // 3. Process incoming UDP packet
        struct sockaddr_in src_addr{};
        socklen_t addr_len = sizeof(src_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

        if (len > 0) {
            uint8_t packet_id = rx_buffer[0];

            if (g_device_state == DeviceState::UNINITIALIZED &&
                packet_id == static_cast<uint8_t>(ZypherProtocol::PacketType::MASTER_CAPTURE)) {

                if (len >= sizeof(ZypherProtocol::CapturePacket)) {
                    auto* capture = reinterpret_cast<ZypherProtocol::CapturePacket*>(rx_buffer);

                    // Compute hash locally using shared mathematical key logic
                    uint64_t expected_hash = ZypherProtocol::compute_response(g_current_challenge, ZypherProtocol::PRE_SHARED_SECRET);

                    if (capture->challenge_response == expected_hash) {
                        // Handshake Succeeded! Initialize and secure control session
                        g_device_state = DeviceState::CONTROLLED;
                        g_session_token = capture->challenge_response;
                        g_master_addr = src_addr;
                        std::strncpy(g_locked_master_id, capture->master_id, sizeof(g_locked_master_id));
                        g_last_master_activity = k_uptime_get();

                        LOG_INF("[Core 0] >>> HANDSHAKE SUCCESS! Board CAPTURED by Master '%s' <<<", g_locked_master_id);
                        LOG_INF("[Core 0] Locked Master IP: %d.%d.%d.%d:%d",
                                (ntohl(g_master_addr.sin_addr.s_addr) >> 24) & 0xFF,
                                (ntohl(g_master_addr.sin_addr.s_addr) >> 16) & 0xFF,
                                (ntohl(g_master_addr.sin_addr.s_addr) >> 8) & 0xFF,
                                ntohl(g_master_addr.sin_addr.s_addr) & 0xFF,
                                ntohs(g_master_addr.sin_port));

                        // Lock LED state
                        GpioManager::get_instance().set_status_led(true);
                    } else {
                        LOG_WRN("[Core 0] Handshake REJECTED! Bad key response. Expected: 0x%llX, Got: 0x%llX",
                                expected_hash, capture->challenge_response);
                    }
                }
            } else if (g_device_state == DeviceState::CONTROLLED &&
                       packet_id == static_cast<uint8_t>(ZypherProtocol::PacketType::MASTER_CONTROL)) {

                if (len >= sizeof(ZypherProtocol::ControlPacket)) {
                    auto* ctrl = reinterpret_cast<ZypherProtocol::ControlPacket*>(rx_buffer);

                    // Multi-factor verification: Token, IP matching, and Master ID
                    if (ctrl->session_token == g_session_token &&
                        std::strcmp(ctrl->master_id, g_locked_master_id) == 0 &&
                        src_addr.sin_addr.s_addr == g_master_addr.sin_addr.s_addr) {

                        // Heartbeat/Keep-alive verified
                        g_last_master_activity = k_uptime_get();

                        // Set Status LED config
                        if (ctrl->status_led == 1) {
                            GpioManager::get_instance().set_status_led(true);
                        } else if (ctrl->status_led == 0) {
                            GpioManager::get_instance().set_status_led(false);
                        }

                        // Apply RGB levels
                        GpioManager::get_instance().set_rgb_led(ctrl->led_r, ctrl->led_g, ctrl->led_b);

                        LOG_DBG("[Core 0] Control Update applied: R=%d, G=%d, B=%d", ctrl->led_r, ctrl->led_g, ctrl->led_b);
                    } else {
                        LOG_WRN("[Core 0] Warning: Rejected unauthorized master control packet!");
                    }
                }
            }
        }

        k_sleep(K_MSEC(10));
    }

    close(sock);
}

// --- Thread [Core 1]: Core Control, Blinking, and Button Monitor ---
void ctrl_thread_entry(void *p1, void *p2, void *p3) {
    LOG_INF("[Core 1] Hardware Control thread started and successfully pinned to Core 1!");

    bool blink_toggle = false;

    while (true) {
        DeviceState state = g_device_state.load();

        if (state == DeviceState::UNINITIALIZED) {
            // Rapid Status LED blinking (250ms interval) to visually represent "uninitialized"
            blink_toggle = !blink_toggle;
            GpioManager::get_instance().set_status_led(blink_toggle);
            k_sleep(K_MSEC(250));
        } else {
            // While Controlled: monitor inputs & print diagnostics
            bool btn_pressed = GpioManager::get_instance().get_button_state();
            if (btn_pressed) {
                LOG_INF("[Core 1] Local Button press detected!");
            }
            k_sleep(K_MSEC(200));
        }
    }
}

// --- Zephyr App Entry Point ---
void main() {
    LOG_INF("--- Initializing Zypher S3 RTOS Station Node ---");

    // Initialize low-level GPIO
    if (!GpioManager::get_instance().initialize()) {
        LOG_ERR("Fatal: Failed to initialize GpioManager!");
        return;
    }
    LOG_INF("GpioManager successfully initialized.");

    // Spawn and pin Core 0 Thread (Network & Sockets)
    k_tid_t net_tid = k_thread_create(&net_thread_data, net_thread_stack,
                                      K_THREAD_STACK_SIZEOF(net_thread_stack),
                                      net_thread_entry, nullptr, nullptr, nullptr,
                                      THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_cpu_pin(net_tid, 0);

    // Spawn and pin Core 1 Thread (Hardware Processing & Loop)
    k_tid_t ctrl_tid = k_thread_create(&ctrl_thread_data, ctrl_thread_stack,
                                       K_THREAD_STACK_SIZEOF(ctrl_thread_stack),
                                       ctrl_thread_entry, nullptr, nullptr, nullptr,
                                       THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_cpu_pin(ctrl_tid, 1);

    LOG_INF("Dual-Core Scheduling configured: net_thread pinned to CPU 0, ctrl_thread pinned to CPU 1.");
}
