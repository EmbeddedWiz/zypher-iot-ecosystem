/**
 * @file protocol.hpp
 * @brief Zypher Binary Protocol Specification (Cross-Platform).
 *
 * This header defines the wire format and cryptographic logic for the Zypher
 * ecosystem. It is designed to be the "Single Source of Truth" for both
 * the C++17 (ESP32) and Kotlin (Android) environments.
 *
 * DESIGN RATIONALE:
 * 1. Binary Efficiency: Fixed-size structures are used to eliminate the
 *    computational and bandwidth overhead of text-based protocols (JSON/XML).
 * 2. Determinism: Strict memory layout ensures predictable behavior in
 *    safety-critical embedded contexts.
 * 3. Security: A challenge-response mechanism protects against replay attacks.
 *
 * compliance: Follows MISRA C++:2023 and Industrial IoT safety standards.
 */

#pragma once

#include <cstdint>

/**
 * @namespace ZypherProtocol
 * @brief Encapsulates all protocol-specific constants, types, and algorithms.
 */
namespace ZypherProtocol {

    /** @brief Pre-Shared Key (PSK) used as a secret factor in the hash mixer. */
    constexpr uint64_t PRE_SHARED_SECRET = 0x5A59504845525333ULL; // "ZYPHERS3"

    /** @brief Standardized UDP port for system-wide communication. */
    constexpr uint16_t BROADCAST_PORT    = 1234U;

    /**
     * @enum PacketType
     * @brief Fixed-width packet identifiers for efficient switch-case dispatch.
     */
    enum class PacketType : uint8_t {
        DISCOVERY_ANNOUNCEMENT = 0x01U, /**< Slave -> Master: Periodic heartbeat in standby */
        MASTER_CAPTURE         = 0x02U, /**< Master -> Slave: Auth request to lock device */
        MASTER_CONTROL         = 0x03U, /**< Master -> Slave: Real-time GPIO/PWM commands */
        TELEMETRY_REPORT       = 0x04U, /**< Slave -> Master: High-frequency status report */
        MASTER_RELEASE         = 0x05U, /**< Master -> Slave: Intentional session termination */
        CAPTURE_ACK            = 0x06U, /**< Slave -> Master: Handshake confirmation */
        RELEASE_ACK            = 0x07U  /**< Slave -> Master: Release confirmation */
    };

    /**
     * Forced 1-byte alignment to prevent compiler-added padding.
     * This ensures binary parity between JVM (Big/Little Endian managed)
     * and Xtensa/C++ memory maps.
     */
    #pragma pack(push, 1)

    /**
     * @struct DiscoveryPacket
     * @brief Broadcast frame sent by the device to announce its presence.
     */
    struct DiscoveryPacket {
        uint8_t  packet_type = static_cast<uint8_t>(PacketType::DISCOVERY_ANNOUNCEMENT);
        char     board_name[16];      /**< Human-readable device name (Null-terminated) */
        char     service_class[16];   /**< Functional category (e.g. LED_CTRL) */
        char     serial_number[16];   /**< Unique hardware UID */
        char     mac_address[18];     /**< String representation of STA MAC */
        uint64_t challenge_nonce;     /**< Hardware-TRNG generated 64-bit challenge */
    };

    /**
     * @struct CapturePacket
     * @brief Unicast frame sent by Master to gain exclusive control.
     */
    struct CapturePacket {
        uint8_t  packet_type = static_cast<uint8_t>(PacketType::MASTER_CAPTURE);
        char     master_id[16];       /**< Identifier of the controlling master node */
        uint64_t challenge_response;  /**< Mixed result of challenge_nonce + PSK */
    };

    /**
     * @struct ControlPacket
     * @brief Main control payload for real-time hardware mapping.
     */
    struct ControlPacket {
        uint8_t  packet_type = static_cast<uint8_t>(PacketType::MASTER_CONTROL);
        char     master_id[16];       /**< Verification of the session owner */
        uint64_t session_token;       /**< Dynamic token to validate every command */
        uint8_t  status_led;          /**< Discrete state (0=OFF, 1=ON) */
        uint8_t  led_r;               /**< 8-bit Red channel PWM duty cycle */
        uint8_t  led_g;               /**< 8-bit Green channel PWM duty cycle */
        uint8_t  led_b;               /**< 8-bit Blue channel PWM duty cycle */
    };

    /**
     * @struct TelemetryPacket
     * @brief Monitoring frame providing hardware observability.
     */
    struct TelemetryPacket {
        uint8_t  packet_type = static_cast<uint8_t>(PacketType::TELEMETRY_REPORT);
        uint32_t uptime_seconds;      /**< Total active time since last reset */
        float    cpu_temperature;     /**< Internal die temperature in Celsius */
        uint8_t  button_state;        /**< GPIO 0 physical state (0=Rel, 1=Pres) */
        uint8_t  core_0_load;         /**< Load percentage of the Comms core */
        uint8_t  core_1_load;         /**< Load percentage of the Hardware core */
    };

    #pragma pack(pop)

    /**
     * @brief Cryptographic Challenge-Response Algorithm.
     *
     * Implements a custom hybrid of FNV-1a (Non-cryptographic hash) and
     * MurmurHash3 finalizer to achieve high entropy and avalanche effects
     * with minimal CPU cycles.
     *
     * @note This function is 'inline' to allow header-only distribution
     * and minimize call overhead in high-frequency loops.
     *
     * @param challenge 64-bit random nonce from the target device.
     * @param secret 64-bit pre-shared secret known only to the ecosystem.
     * @return 64-bit computed response token.
     *
     * @note This lightweight implementation can be replaced with a stronger algorithm (e.g., HMAC‑SHA256, AES‑GCM, or a TLS/DTLS based authentication) without modifying the surrounding protocol.
     */
    inline uint64_t compute_response(const uint64_t challenge, const uint64_t secret) {
        uint64_t hash = 0xcbf29ce484222325ULL; /**< FNV-1a 64-bit offset basis */
        constexpr uint64_t prime = 0x100000001b3ULL; /**< FNV-1a 64-bit prime */

        // --- PHASE 1: FNV-1a XOR-MULTIPLY ---
        hash ^= challenge;
        hash *= prime;
        hash ^= secret;
        hash *= prime;

        // --- PHASE 2: MURMURHASH3 MIXER (Avalanche Protection) ---
        // These constants are optimized for 64-bit bit distribution.
        hash ^= (hash >> 33U);
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= (hash >> 33U);
        hash *= 0xc4ceb9fe1a85ec53ULL;
        hash ^= (hash >> 33U);

        return hash;
    }

} // namespace ZypherProtocol
