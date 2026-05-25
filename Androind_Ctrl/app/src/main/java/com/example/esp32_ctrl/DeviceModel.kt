/**
 * @file DeviceModel.kt
 * @brief Domain models for the Zypher IoT Ecosystem.
 *
 * Implements a reactive data model for remote hardware devices.
 * Uses Jetpack Compose state management to ensure seamless UI-hardware synchronization.
 */

package com.example.esp32_ctrl

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * @class SmartDeviceControl
 * @brief Abstract base class for all controllable hardware units.
 *
 * Encapsulates common network properties and lifecycle states.
 * Reactive properties (mutableStateOf) ensure the UI updates automatically upon telemetry arrival.
 */
abstract class SmartDeviceControl(
    val id: String,           /**< Unique Hardware Serial Number / UID */
    val initialName: String,  /**< Factory-assigned display name */
    val deviceClass: String   /**< Hardware capability identifier (e.g., "ESP32S3") */
) {
    /** Networking Identifiers */
    var ipAddress by mutableStateOf("")
    var macAddress by mutableStateOf("")
    
    /** Lifecycle Management */
    var lastSeen by mutableLongStateOf(0L)  /**< Timestamp of the last received datagram (ms) */
    var isOnline by mutableStateOf(false)   /**< Availability status in the local network */
    var isConnected by mutableStateOf(false) /**< Active cryptographic session status */
    
    var name by mutableStateOf(initialName)

    /** visual representation */
    abstract val icon: ImageVector

    /** 
     * @brief Renders the specialized control dashboard for the specific hardware class.
     * Implemented by concrete subclasses to provide hardware-specific UX.
     */
    @Composable
    abstract fun ControlPanel()
}

/**
 * @class Esp32S3Control
 * @brief Specialized implementation for ESP32-S3 based LED controllers.
 *
 * Manages the specific feature set of the Zypher S3 firmware:
 * 1. Challenge-Response security tokens.
 * 2. Real-time telemetry (CPU Temp, Uptime, GPIO events).
 * 3. 24-bit RGB channel mapping.
 */
class Esp32S3Control(id: String, name: String) : SmartDeviceControl(id, name, "ESP32S3-WROOM-1.0") {
    override val icon = Icons.Default.Settings 

    /** Security & Handshake state */
    var challengeNonce by mutableLongStateOf(0L) /**< Random nonce received via Discovery */
    var sessionToken by mutableLongStateOf(0L)   /**< Computed cryptographic token for current session */

    /** Hardware Telemetry (Inbound) */
    var coreTemp by mutableStateOf("0.0 °C")
    var bootButtonPressed by mutableStateOf(false)
    var uptime by mutableLongStateOf(0L)

    /** Hardware Commands (Outbound) */
    var r by mutableFloatStateOf(0.0f)
    var g by mutableFloatStateOf(0.0f)
    var b by mutableFloatStateOf(0.0f)

    /**
     * @brief AUSTERE DESIGN PHILOSOPHY:
     * The Control Panel follows an "Industrial Dashboard" aesthetic - 
     * high information density, high contrast, and minimal decorative elements.
     */
    @Composable
    override fun ControlPanel() {
        Column(modifier = Modifier.padding(8.dp)) {
            Text(
                text = "$name System Control", 
                style = MaterialTheme.typography.headlineSmall, 
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.SemiBold
            )
            
            Spacer(modifier = Modifier.height(16.dp))
            
            /** Hardware Status Row (Telemetry visualization) */
            Row(
                modifier = Modifier.fillMaxWidth(), 
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                StatusCard(Modifier.weight(1f), "BOOT BTN", if (bootButtonPressed) "ACTIVE" else "IDLE", if (bootButtonPressed) Color.Red else Color.Unspecified)
                StatusCard(Modifier.weight(1f), "DIE TEMP", coreTemp)
                StatusCard(Modifier.weight(1f), "UPTIME", "${uptime}s")
            }

            Spacer(modifier = Modifier.height(24.dp))
            
            Text("Light Management Interface", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
            Spacer(modifier = Modifier.height(8.dp))
            
            /** 24-bit Color Mapping Sliders */
            ColorSlider("Red Channel", r, Color.Red) { r = it }
            ColorSlider("Green Channel", g, Color.Green) { g = it }
            ColorSlider("Blue Channel", b, Color.Blue) { b = it }
            
            Spacer(modifier = Modifier.height(12.dp))

            /** Visual Hardware Simulation (Preview) */
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("LIVE STATE", modifier = Modifier.width(70.dp), fontSize = 10.sp, fontWeight = FontWeight.Bold, color = Color.Gray)
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(44.dp)
                        .background(Color(r, g, b), RoundedCornerShape(4.dp))
                        .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(4.dp))
                )
            }
        }
    }
}

/**
 * @brief Industrial-style information card for telemetry data.
 */
@Composable
fun StatusCard(modifier: Modifier = Modifier, label: String, value: String, valueColor: Color = Color.Unspecified) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(4.dp), // Sharper corners for industrial look
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f))
    ) {
        Column(
            modifier = Modifier.padding(8.dp), 
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(label, fontSize = 9.sp, fontWeight = FontWeight.Light, color = Color.Gray)
            Text(value, fontSize = 14.sp, fontWeight = FontWeight.Bold, color = valueColor)
        }
    }
}

/**
 * @brief High-precision control slider with channel-specific color feedback.
 */
@Composable
fun ColorSlider(label: String, value: Float, color: Color, onValueChange: (Float) -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)
    ) {
        Text(
            text = label, 
            modifier = Modifier.width(85.dp), 
            fontSize = 11.sp,
            fontWeight = FontWeight.Medium,
            color = Color.Gray
        )
        Slider(
            value = value, 
            onValueChange = onValueChange, 
            modifier = Modifier.weight(1f),
            colors = SliderDefaults.colors(
                thumbColor = color, 
                activeTrackColor = color.copy(alpha = 0.4f),
                inactiveTrackColor = Color.LightGray.copy(alpha = 0.2f)
            )
        )
    }
}
