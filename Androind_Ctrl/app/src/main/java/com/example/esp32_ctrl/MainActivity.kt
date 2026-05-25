/**
 * @file MainActivity.kt
 * @brief Zypher Master Node: High-performance dashboard and network coordinator.
 *
 * This class implements the control logic for a heterogeneous IoT network. 
 * It manages asynchronous datagram processing on dedicated worker threads 
 * and provides a reactive user interface via Jetpack Compose.
 */

package com.example.esp32_ctrl

import android.Manifest
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.lifecycleScope
import com.example.esp32_ctrl.ui.theme.ESP32_CtrlTheme
import kotlinx.coroutines.*
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

/**
 * @object AppConfig
 * @brief Functional parameters and timing constants.
 */
object AppConfig {
    const val LOG_TAG = "Zypher_App"
    
    /** Watchdog parameters (observability) */
    const val OFFLINE_TIMEOUT_MS = 12000L /**< Silent node grace period */
    const val REMOVE_TIMEOUT_MS  = 42000L /**< Persistence purge period */
    
    /** Real-time control parameters */
    const val CONTROL_TICK_MS    = 300L   /**< Actuator command frequency (~3.3Hz) */
    const val SPLASH_DELAY_MS    = 1800L
}

enum class ScreenState { SPLASH, LIST }

/**
 * @class MainActivity
 * @brief Orchestrates system-level tasks: OS permissions, network streams, and UI state.
 */
class MainActivity : ComponentActivity() {

    private lateinit var multicastLock: WifiManager.MulticastLock
    private var locationPermissionGranted = mutableStateOf(false)
    
    /** Persistent UDP socket shared across all coroutine pipelines */
    private var mainSocket: DatagramSocket? = null
    
    /** Reactive inventory of discovered hardware nodes */
    private val devices = mutableStateListOf<SmartDeviceControl>()

    /** Modern Activity Result launcher for dynamic permission requests */
    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted -> locationPermissionGranted.value = isGranted }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        /**
         * OS PERMISSIONS:
         * Android 12+ requires explicit Location permission for scanning 
         * the local network and resolving hardware broadcast packets.
         */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
        } else {
            locationPermissionGranted.value = true
        }

        /**
         * RADIO POWER MANAGEMENT:
         * We acquire a MulticastLock to ensure the device remains sensitive 
         * to incoming UDP Broadcast datagrams from the ESP32 units.
         */
        val wifi = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("zypher_udp_lock").apply {
            setReferenceCounted(true)
            acquire()
        }

        /** Initialize the asynchronous network coordinator */
        startNetworkEngine()

        setContent {
            var currentScreen by remember { mutableStateOf(ScreenState.SPLASH) }
            val systemTheme = isSystemInDarkTheme()
            var isDarkMode by remember { mutableStateOf(systemTheme) }
            
            ESP32_CtrlTheme(darkTheme = isDarkMode) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    when (currentScreen) {
                        ScreenState.SPLASH -> SplashScreen { currentScreen = ScreenState.LIST }
                        ScreenState.LIST -> MainScreen(
                            devices = devices,
                            isDarkMode = isDarkMode,
                            onThemeToggle = { isDarkMode = !isDarkMode },
                            onConnectClick = { if (it is Esp32S3Control) sendCapturePacket(it) },
                            onDisconnectClick = { if (it is Esp32S3Control) sendReleasePacket(it) }
                        )
                    }
                }
            }
        }
    }

    /**
     * @brief CONCURRENCY ARCHITECTURE:
     * To maintain sub-10ms UI responsiveness, the networking engine is divided
     * into three non-blocking pipelines using Kotlin Coroutines.
     */
    private fun startNetworkEngine() {
        lifecycleScope.launch(Dispatchers.IO) {
            val socket = try {
                DatagramSocket(ZypherProtocol.BROADCAST_PORT).apply {
                    broadcast = true
                    soTimeout = 1000
                }
            } catch (e: Exception) {
                Log.e(AppConfig.LOG_TAG, "Critical: UDP socket init error")
                return@launch
            }
            mainSocket = socket

            val rxBuffer = ByteArray(512)

            /**
             * PIPELINE 1: INBOUND RX
             * Continuously monitors the socket for Discovery and Telemetry frames.
             * Implements strict buffer isolation for thread safety.
             */
            launch {
                while (isActive) {
                    try {
                        val packet = DatagramPacket(rxBuffer, rxBuffer.size)
                        socket.receive(packet)
                        
                        // Buffer copying ensures state consistency during parsing
                        val payload = packet.data.copyOfRange(0, packet.length)
                        val data = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                        if (payload.isEmpty()) continue

                        val type = data.get()
                        val ip = packet.address.hostAddress ?: continue

                        when (type) {
                            ZypherProtocol.PacketType.DISCOVERY.id -> {
                                val name = readFixedString(data, 16)
                                val service = readFixedString(data, 16)
                                val serial = readFixedString(data, 16)
                                val mac = readFixedString(data, 18)
                                val nonce = if (data.remaining() >= 8) data.long else 0L

                                withContext(Dispatchers.Main) {
                                    val dev = devices.find { it.id == serial } 
                                        ?: Esp32S3Control(serial, name).also { devices.add(it) }
                                    
                                    dev.ipAddress = ip
                                    dev.macAddress = mac
                                    dev.isOnline = true
                                    dev.lastSeen = System.currentTimeMillis()
                                    
                                    // Security: Only update challenge when session is idle
                                    if (!dev.isConnected) {
                                        (dev as? Esp32S3Control)?.challengeNonce = nonce
                                    }
                                }
                            }
                            
                            ZypherProtocol.PacketType.TELEMETRY.id -> {
                                withContext(Dispatchers.Main) {
                                    val dev = devices.find { it.ipAddress == ip }
                                    if (dev is Esp32S3Control) {
                                        dev.lastSeen = System.currentTimeMillis()
                                        dev.isOnline = true

                                        if (data.remaining() >= 8) {
                                            dev.uptime = data.int.toLong()
                                            dev.coreTemp = String.format(Locale.US, "%.1f °C", data.float)
                                            dev.bootButtonPressed = (data.get().toInt() == 1)
                                            
                                            // Synchronize UI sliders with current hardware state
                                            if (!dev.isConnected && data.remaining() >= 3) {
                                                dev.r = data.get().toUByte().toInt() / 255f
                                                dev.g = data.get().toUByte().toInt() / 255f
                                                dev.b = data.get().toUByte().toInt() / 255f
                                            }
                                        }
                                    }
                                }
                            }

                            ZypherProtocol.PacketType.CAPTURE_ACK.id -> {
                                withContext(Dispatchers.Main) {
                                    devices.find { it.ipAddress == ip }?.isConnected = true
                                }
                            }

                            ZypherProtocol.PacketType.RELEASE_ACK.id -> {
                                withContext(Dispatchers.Main) {
                                    val dev = devices.find { it.ipAddress == ip }
                                    if (dev is Esp32S3Control) {
                                        dev.isConnected = false
                                        dev.sessionToken = 0L
                                    }
                                }
                            }
                        }
                    } catch (e: Exception) {
                        if (e !is java.net.SocketTimeoutException) {
                            Log.e(AppConfig.LOG_TAG, "RX Pipeline error")
                        }
                    }
                }
            }

            /**
             * PIPELINE 2: OUTBOUND TX
             * Periodically pushes control payloads to captured nodes.
             * Throttled to 300ms to balance responsiveness and hardware stack load.
             */
            launch {
                while (isActive) {
                    val targets = withContext(Dispatchers.Main) { 
                        devices.filter { it.isConnected && it is Esp32S3Control }.map { it as Esp32S3Control }
                    }

                    targets.forEach { dev ->
                        try {
                            val msg = ByteBuffer.allocate(29).order(ByteOrder.LITTLE_ENDIAN)
                            msg.put(ZypherProtocol.PacketType.CONTROL.id)
                            msg.put("Android-Node-01".toByteArray().padEnd(16))
                            msg.putLong(dev.sessionToken)
                            msg.put(1.toByte()) // Status: Active
                            msg.put((dev.r * 255).toInt().toByte())
                            msg.put((dev.g * 255).toInt().toByte())
                            msg.put((dev.b * 255).toInt().toByte())

                            val p = DatagramPacket(msg.array(), msg.capacity(), 
                                InetSocketAddress(dev.ipAddress, ZypherProtocol.BROADCAST_PORT))
                            socket.send(p)
                        } catch (e: Exception) {
                            Log.e(AppConfig.LOG_TAG, "TX Pipeline error")
                        }
                    }
                    delay(AppConfig.CONTROL_TICK_MS)
                }
            }

            /**
             * PIPELINE 3: NODE SUPERVISION
             * Periodically audits the node inventory to prune offline or unresponsive devices.
             */
            launch {
                while (isActive) {
                    delay(2000L)
                    val now = System.currentTimeMillis()
                    withContext(Dispatchers.Main) {
                        val iterator = devices.iterator()
                        while (iterator.hasNext()) {
                            val d = iterator.next()
                            val delta = now - d.lastSeen
                            
                            if (delta > AppConfig.REMOVE_TIMEOUT_MS) {
                                iterator.remove()
                                continue
                            }

                            if (delta > AppConfig.OFFLINE_TIMEOUT_MS) {
                                d.isOnline = false
                                if (d.isConnected) {
                                    d.isConnected = false
                                    (d as? Esp32S3Control)?.sessionToken = 0L
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    private fun sendCapturePacket(device: Esp32S3Control) {
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                val socket = mainSocket ?: return@launch
                val response = ZypherProtocol.computeResponse(device.challengeNonce, ZypherProtocol.PRE_SHARED_SECRET)
                device.sessionToken = response
                
                val buffer = ByteBuffer.allocate(25).order(ByteOrder.LITTLE_ENDIAN)
                buffer.put(ZypherProtocol.PacketType.CAPTURE.id)
                buffer.put("Android-Node-01".toByteArray().padEnd(16))
                buffer.putLong(response)

                val packet = DatagramPacket(buffer.array(), buffer.capacity(), 
                    InetSocketAddress(device.ipAddress, ZypherProtocol.BROADCAST_PORT))
                
                socket.send(packet)
            } catch (e: Exception) {
                Log.e(AppConfig.LOG_TAG, "Handshake request failure")
            }
        }
    }

    private fun sendReleasePacket(device: Esp32S3Control) {
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                val socket = mainSocket ?: return@launch
                val buffer = ByteBuffer.allocate(25).order(ByteOrder.LITTLE_ENDIAN)
                buffer.put(ZypherProtocol.PacketType.RELEASE.id)
                buffer.put("Android-Node-01".toByteArray().padEnd(16))
                buffer.putLong(device.sessionToken) 

                val packet = DatagramPacket(buffer.array(), buffer.capacity(), 
                    InetSocketAddress(device.ipAddress, ZypherProtocol.BROADCAST_PORT))
                socket.send(packet)
            } catch (e: Exception) {
                Log.e(AppConfig.LOG_TAG, "Release request failure")
            }
        }
    }

    private fun readFixedString(buffer: ByteBuffer, length: Int): String {
        val bytes = ByteArray(length)
        buffer.get(bytes)
        return String(bytes).takeWhile { it != '\u0000' }
    }

    private fun ByteArray.padEnd(size: Int): ByteArray {
        val res = ByteArray(size)
        System.arraycopy(this, 0, res, 0, minOf(this.size, size))
        return res
    }

    override fun onDestroy() {
        super.onDestroy()
        mainSocket?.close()
        if (this::multicastLock.isInitialized && multicastLock.isHeld) {
            multicastLock.release()
        }
    }
}

/**
 * @brief Procedural Vector Logo:
 * Represents an IoT node using gexa-grid geometry.
 * Drawn via Canvas to eliminate raster assets and ensure resolution independence.
 */
@Composable
fun TechnicalLogo(modifier: Modifier = Modifier, color: Color = MaterialTheme.colorScheme.primary) {
    Canvas(modifier = modifier) {
        val w = size.width
        val h = size.height
        val strokeWidth = 3.dp.toPx()

        val hexPath = Path().apply {
            moveTo(w * 0.5f, 0f)
            lineTo(w, h * 0.25f)
            lineTo(w, h * 0.75f)
            lineTo(w * 0.5f, h)
            lineTo(0f, h * 0.75f)
            lineTo(0f, h * 0.25f)
            close()
        }
        drawPath(hexPath, color = color, style = Stroke(width = strokeWidth))
        drawCircle(color = color, radius = w * 0.15f, center = center)

        drawLine(color = color, start = center, end = androidx.compose.ui.geometry.Offset(w * 0.5f, h * 0.15f), strokeWidth = strokeWidth * 0.6f)
        drawLine(color = color, start = center, end = androidx.compose.ui.geometry.Offset(w * 0.15f, h * 0.7f), strokeWidth = strokeWidth * 0.6f)
        drawLine(color = color, start = center, end = androidx.compose.ui.geometry.Offset(w * 0.85f, h * 0.7f), strokeWidth = strokeWidth * 0.6f)
        
        drawCircle(color = color, radius = w * 0.05f, center = androidx.compose.ui.geometry.Offset(w * 0.5f, h * 0.15f))
        drawCircle(color = color, radius = w * 0.05f, center = androidx.compose.ui.geometry.Offset(w * 0.15f, h * 0.7f))
        drawCircle(color = color, radius = w * 0.05f, center = androidx.compose.ui.geometry.Offset(w * 0.85f, h * 0.7f))
    }
}

@Composable
fun SplashScreen(onTimeout: () -> Unit) {
    LaunchedEffect(Unit) {
        delay(AppConfig.SPLASH_DELAY_MS)
        onTimeout()
    }

    Box(modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            TechnicalLogo(modifier = Modifier.size(140.dp))
            Spacer(modifier = Modifier.height(48.dp))
            Text(
                text = "ZYPHER ECOSYSTEM", 
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Light,
                letterSpacing = 6.sp,
                color = MaterialTheme.colorScheme.primary.copy(alpha = 0.8f)
            )
            LinearProgressIndicator(
                modifier = Modifier.padding(top = 24.dp).width(120.dp).height(2.dp),
                color = MaterialTheme.colorScheme.primary,
                trackColor = MaterialTheme.colorScheme.surfaceVariant
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    devices: List<SmartDeviceControl>,
    isDarkMode: Boolean, 
    onThemeToggle: () -> Unit,
    onConnectClick: (SmartDeviceControl) -> Unit,
    onDisconnectClick: (SmartDeviceControl) -> Unit
) {
    var selectedId by remember { mutableStateOf<String?>(null) }
    val selectedDevice = devices.find { it.id == selectedId }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Zypher Device Inventory", fontWeight = FontWeight.Bold) },
                actions = {
                    IconButton(onClick = onThemeToggle) {
                        Icon(
                            imageVector = if (isDarkMode) Icons.Default.Info else Icons.Default.Settings,
                            contentDescription = "Theme Toggle"
                        )
                    }
                }
            )
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier.fillMaxSize().padding(innerPadding).padding(16.dp)
        ) {
            Text(
                text = "HARDWARE REGISTRY", 
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.Bold,
                color = Color.Gray
            )
            Spacer(modifier = Modifier.height(12.dp))

            Box(
                modifier = Modifier.height(220.dp).fillMaxWidth()
                    .border(1.dp, Color.DarkGray, RoundedCornerShape(4.dp))
                    .clip(RoundedCornerShape(4.dp))
            ) {
                val scrollState = rememberScrollState()
                Column(modifier = Modifier.horizontalScroll(scrollState)) {
                    Row(modifier = Modifier.background(Color(0xFF222222)).padding(vertical = 12.dp)) {
                        TableHeader("*", 35.dp)
                        TableHeader("IDENTIFIER", 100.dp)
                        TableHeader("CLASS", 120.dp)
                        TableHeader("UID/SERIAL", 110.dp)
                        TableHeader("NETWORK IP", 100.dp)
                        TableHeader("PHYSICAL MAC", 140.dp)
                    }
                    HorizontalDivider(color = Color.DarkGray)

                    LazyColumn(modifier = Modifier.fillMaxSize()) {
                        items(devices) { dev ->
                            val isSelected = selectedId == dev.id
                            Row(
                                modifier = Modifier.clickable { selectedId = dev.id }
                                    .background(if (isSelected) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.3f) else Color.Transparent)
                                    .padding(vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Box(modifier = Modifier.width(35.dp), contentAlignment = Alignment.Center) {
                                    val icon = when {
                                        dev.isConnected -> Icons.Default.CheckCircle
                                        dev.isOnline -> Icons.Default.Refresh
                                        else -> Icons.Default.Info
                                    }
                                    val color = when {
                                        dev.isConnected -> Color(0xFF4CAF50)
                                        dev.isOnline -> Color(0xFFFFA000)
                                        else -> Color(0xFFF44336)
                                    }
                                    Icon(icon, null, Modifier.size(16.dp), tint = color)
                                }
                                TableCell(dev.name, 100.dp, isSelected)
                                TableCell(dev.deviceClass, 120.dp, isSelected)
                                TableCell(dev.id, 110.dp, isSelected)
                                TableCell(dev.ipAddress, 100.dp, isSelected)
                                TableCell(dev.macAddress, 140.dp, isSelected)
                            }
                            HorizontalDivider(color = Color.Gray.copy(alpha = 0.2f))
                        }
                    }
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            Button(
                onClick = { selectedDevice?.let { if (it.isConnected) onDisconnectClick(it) else onConnectClick(it) } },
                enabled = selectedId != null,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(4.dp)
            ) {
                val label = if (selectedDevice?.isConnected == true) "RELEASE LOCK" else "ACQUIRE CONTROL"
                Text(label, fontWeight = FontWeight.Bold)
            }

            Spacer(modifier = Modifier.height(24.dp))

            Box(modifier = Modifier.weight(1f)) {
                if (selectedDevice?.isConnected == true) {
                    selectedDevice.ControlPanel()
                } else {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text(
                            text = "AWAITING AUTHORIZATION",
                            style = MaterialTheme.typography.labelSmall,
                            color = Color.Gray
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun TableHeader(text: String, width: androidx.compose.ui.unit.Dp) {
    Text(text, modifier = Modifier.width(width), fontWeight = FontWeight.Black, fontSize = 10.sp, color = Color.LightGray, textAlign = TextAlign.Center)
}

@Composable
fun TableCell(text: String, width: androidx.compose.ui.unit.Dp, isSelected: Boolean) {
    Text(text, modifier = Modifier.width(width), fontSize = 11.sp, color = if (isSelected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface, textAlign = TextAlign.Center, maxLines = 1)
}
