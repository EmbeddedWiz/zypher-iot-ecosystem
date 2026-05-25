package com.example.esp32_ctrl

import java.nio.ByteBuffer

enum class DeviceClass {
    LED_STRIP,
    RGB_LAMP,
    CUSTOM
}

/**
 * Encode a LedCommand to a ByteArray.
 * For now all device classes use the same format (red, green, blue as UInt32).
 * This function can be extended later to support device‑specific payloads.
 */
fun LedCommand.toByteArray(deviceClass: DeviceClass = DeviceClass.LED_STRIP): ByteArray {
    return ByteBuffer.allocate(12)
        .putInt(red.toInt())
        .putInt(green.toInt())
        .putInt(blue.toInt())
        .array()
}
