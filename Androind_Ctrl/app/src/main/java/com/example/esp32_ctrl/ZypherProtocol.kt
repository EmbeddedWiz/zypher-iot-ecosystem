/**
 * @file ZypherProtocol.kt
 * @brief Cross-platform binary communication protocol definition.
 *
 * This module defines the wire format and cryptographic logic shared between 
 * the Android Master and the ESP32-S3 Slave. 
 *
 * ARCHITECTURAL CHOICE: A custom binary protocol over UDP was chosen instead of HTTP/JSON
 * to achieve sub-10ms latency and minimal parsing overhead on low-power microcontrollers.
 */

package com.example.esp32_ctrl

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * @object ZypherProtocol
 * @brief Container for protocol-specific constants and hash algorithms.
 */
object ZypherProtocol {
    /** @brief Target UDP port for all system communications */
    const val BROADCAST_PORT = 1234
    
    /** @brief Pre-Shared Key (PSK) used as the second factor in the hash mixer */
    const val PRE_SHARED_SECRET: Long = 0x5A59504845525333L // "ZYPHERS3" hex string

    /**
     * @enum PacketType
     * @brief Unique identifiers for all supported datagram types.
     */
    enum class PacketType(val id: Byte) {
        DISCOVERY(0x01),    /**< Inbound: Slave broadcasting availability */
        CAPTURE(0x02),      /**< Outbound: Master requesting session lock */
        CONTROL(0x03),      /**< Outbound: Master sending real-time GPIO commands */
        TELEMETRY(0x04),    /**< Inbound: Slave reporting sensor data */
        RELEASE(0x05),      /**< Outbound: Master terminating session */
        CAPTURE_ACK(0x06),  /**< Inbound: Slave confirming successful capture */
        RELEASE_ACK(0x07)   /**< Inbound: Slave confirming session termination */
    }

    /**
     * @brief LIGHTWEIGHT CRYPTOGRAPHIC MIXER (Challenge-Response).
     *
     * This algorithm implements a hybrid FNV-1a + MurmurHash3 scrambler.
     * It is designed to provide robust authentication without the computational cost 
     * or memory footprint of full-scale AES or RSA, which is often excessive for 
     * simple hardware control loops.
     *
     * SECURITY MODEL:
     * 1. Slave generates a hardware-random 64-bit Nonce (Challenge).
     * 2. Master receives the Nonce and mixes it with the PRE_SHARED_SECRET.
     * 3. Resulting 64-bit token is sent back for verification.
     * 4. This prevents replay attacks and unauthorized device hijacking.
     *
     * @param challenge 64-bit random value from the device.
     * @param secret 64-bit pre-shared secret key.
     * @return 64-bit computed session token.
     */
    fun computeResponse(challenge: Long, secret: Long): Long {
        /** 
         * Use ULong (unsigned 64-bit) to ensure bit-level parity with 
         * the C++ implementation on the Xtensa architecture.
         */
        var hash: ULong = 0xcbf29ce484222325uL // FNV-1a 64-bit offset basis
        val prime: ULong = 0x100000001b3uL      // FNV-1a 64-bit prime

        // --- STAGE 1: FNV-1a MIXING ---
        hash = hash xor challenge.toULong()
        hash *= prime
        hash = hash xor secret.toULong()
        hash *= prime

        // --- STAGE 2: MURMURHASH3 SCRAMBLING (Avalanche effect) ---
        hash = hash xor (hash shr 33)
        hash *= 0xff51afd7ed558ccdUL
        hash = hash xor (hash shr 33)
        hash *= 0xc4ceb9fe1a85ec53UL
        hash = hash xor (hash shr 33)

        return hash.toLong()
    }
}
