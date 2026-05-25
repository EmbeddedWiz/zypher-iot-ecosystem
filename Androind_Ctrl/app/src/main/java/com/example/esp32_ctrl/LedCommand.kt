package com.example.esp32_ctrl

// Data structure to send
data class LedCommand(
    val red: UInt,
    val green: UInt,
    val blue: UInt
)
