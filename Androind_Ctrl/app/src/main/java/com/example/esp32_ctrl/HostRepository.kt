package com.example.esp32_ctrl

import androidx.compose.runtime.mutableStateMapOf

object HostRepository {
    // Map of IP -> last seen timestamp (ms)
    val activeHosts = mutableStateMapOf<String, Long>()
}
