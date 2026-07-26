package com.tcwl.timecode

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {

    private lateinit var bleManager: BleManager

    private val enableBleLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { /* BLE enabled */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        bleManager = BleManager(applicationContext)

        checkPermissions()

        setContent {
            TCWLTheme {
                MainScreen(bleManager)
            }
        }
    }

    override fun onDestroy() {
        bleManager.cleanup()
        super.onDestroy()
    }

    private fun checkPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN)
                != PackageManager.PERMISSION_GRANTED
            ) {
                requestPermissions(
                    arrayOf(
                        Manifest.permission.BLUETOOTH_SCAN,
                        Manifest.permission.BLUETOOTH_CONNECT,
                        Manifest.permission.ACCESS_FINE_LOCATION,
                    ), 100
                )
            }
        } else {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED
            ) {
                requestPermissions(
                    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION), 100
                )
            }
        }
        val btAdapter = BluetoothAdapter.getDefaultAdapter()
        if (btAdapter != null && !btAdapter.isEnabled) {
            enableBleLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        }
    }
}

@Composable
fun TCWLTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Color(0xFF00BCD4),
            onPrimary = Color.Black,
            background = Color(0xFF121212),
            surface = Color(0xFF1E1E1E),
            onBackground = Color.White,
            onSurface = Color.White,
        ),
        content = content
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(bleManager: BleManager) {
    val timecode by bleManager.timecode.collectAsStateWithLifecycle()
    val connectionState by bleManager.connectionState.collectAsStateWithLifecycle()
    val scannedDevices by bleManager.scannedDevices.collectAsStateWithLifecycle()

    var drawerOpen by remember { mutableStateOf(false) }
    var showJamDialog by remember { mutableStateOf(false) }
    var statusMsg by remember { mutableStateOf<String?>(null) }

    val snackbarHostState = remember { SnackbarHostState() }
    val deviceState by bleManager.deviceState.collectAsStateWithLifecycle()

    LaunchedEffect(statusMsg) {
        statusMsg?.let {
            snackbarHostState.showSnackbar(it)
            statusMsg = null
        }
    }

    ModalNavigationDrawer(
        drawerState = rememberDrawerState(DrawerValue.Closed).also {
            LaunchedEffect(drawerOpen) { if (drawerOpen) it.open() else it.close() }
        },
        gesturesEnabled = false,
        drawerContent = {
            ModalDrawerSheet {
                ConfigDrawer(
                    bleManager = bleManager,
                    scannedDevices = scannedDevices,
                    connectionState = connectionState,
                    onClose = { drawerOpen = false },
                    onStatus = { statusMsg = it },
                    onShowJam = { showJamDialog = true },
                    currentTimecode = timecode,
                )
            }
        }
    ) {
        Scaffold(
            snackbarHost = { SnackbarHost(snackbarHostState) },
            topBar = {
                TopAppBar(
                    title = {
                    val name = bleManager.deviceName.collectAsStateWithLifecycle().value
                    Text(if (connectionState == ConnectionState.CONNECTED) name else "TC-WL")
                },
                    actions = {
                        Text(
                            text = when (connectionState) {
                                ConnectionState.DISCONNECTED -> "Disconnected"
                                ConnectionState.SCANNING -> "Scanning..."
                                ConnectionState.CONNECTING -> "Connecting..."
                                ConnectionState.CONNECTED -> "Connected"
                            },
                            color = when (connectionState) {
                                ConnectionState.CONNECTED -> Color(0xFF4CAF50)
                                else -> Color.Gray
                            },
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.padding(end = 8.dp)
                        )
                        IconButton(onClick = { drawerOpen = !drawerOpen }) {
                            Text("☰", color = Color.White)
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface
                    )
                )
            },
            containerColor = MaterialTheme.colorScheme.background
        ) { padding ->
            TimecodeDisplay(
                timecode = timecode,
                isConnected = connectionState == ConnectionState.CONNECTED,
                deviceName = if (connectionState == ConnectionState.CONNECTED)
                    bleManager.deviceName.collectAsStateWithLifecycle().value else "TC-WL",
                masterName = if (connectionState == ConnectionState.CONNECTED) deviceState["conn_name"] else null,
                wifiConnected = if (connectionState == ConnectionState.CONNECTED)
                    deviceState["ip"]?.let { it.isNotEmpty() && it != "0.0.0.0" && it != "(null)" } == true else false,
                onTapTimecode = { showJamDialog = true },
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding)
            )
        }
    }

    if (showJamDialog) {
        JamDialog(
            current = timecode,
            onDismiss = { showJamDialog = false },
            onJam = { dd, hh, mm, ss, ff ->
                bleManager.setTimecode(Timecode(dd = dd, hh = hh, mm = mm, ss = ss, ff = ff))
                bleManager.sendConfig("jam", "$dd $hh $mm $ss $ff")
                statusMsg = "Timecode jammed"
                showJamDialog = false
            }
        )
    }
}

@Composable
fun TimecodeDisplay(timecode: Timecode, deviceName: String = "TC-WL", isConnected: Boolean = true, masterName: String? = null, wifiConnected: Boolean = false, onTapTimecode: (() -> Unit)? = null, modifier: Modifier = Modifier) {
    val isLtcDevice = deviceName.contains("LTC", ignoreCase = true)
    BoxWithConstraints(
        modifier = modifier
            .background(Color(0xFF121212))
            .padding(8.dp),
    ) {
        val tcFontSize = (maxWidth.value / 6.5f).coerceIn(32f, 80f).sp

        Column(
            modifier = Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.SpaceBetween
        ) {
            // ══ Top line: BLE + Wi-Fi + name + battery + runtime ══
            Row(
                modifier = Modifier.fillMaxWidth().height(20.dp).padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("B", color = if (isConnected) Color(0xFF00BCD4) else Color(0xFF444444), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                if (wifiConnected) {
                    Spacer(Modifier.width(4.dp))
                    Text("≡", color = if (isConnected) Color(0xFF888888) else Color(0xFF444444), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                }
                Spacer(Modifier.width(4.dp))
                Text(
                    deviceName,
                    color = if (isConnected) Color(0xFFCCCCCC) else Color(0xFF444444),
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.weight(1f),
                    textAlign = TextAlign.Center,
                )
                Text("[", color = Color(0xFF888888), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                val batFill = if (isConnected && timecode.batteryPct <= 100) (timecode.batteryPct * 5 / 100) else 0
                Text("▓".repeat(batFill.coerceIn(0, 5)).padEnd(5, '░'), color = if (isConnected) Color(0xFF4CAF50) else Color(0xFF444444), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                Text("]", color = Color(0xFF888888), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                Spacer(Modifier.width(2.dp))
                Text(if (isConnected) timecode.runtimeText else "--", color = Color(0xFF888888), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
            }

            // ══ Big timecode ══
            Column(
                modifier = Modifier.fillMaxWidth(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                if (isConnected && timecode.dd > 0) {
                    Text(
                        text = "%02d".format(timecode.dd),
                        color = Color(0xFF888888),
                        fontSize = 28.sp,
                        fontFamily = FontFamily.Monospace,
                    )
                    Spacer(Modifier.height(4.dp))
                }
                Text(
                    text = if (isConnected) timecode.display else "--:--:--:--",
                    color = if (isConnected) Color(0xFF00FF88) else Color(0xFF444444),
                    fontSize = tcFontSize,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    textAlign = TextAlign.Center,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable(enabled = isConnected && onTapTimecode != null) { onTapTimecode?.invoke() },
                )
            }

            // ══ Bottom boxes ══
            Row(
                modifier = Modifier.align(Alignment.CenterHorizontally).padding(horizontal = 4.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val roleText = if (isConnected) if (timecode.isMaster) "MASTER" else "SLAVE" else "---"
                OledBox(text = roleText, color = if (isConnected) Color(0xFF00BCD4) else Color(0xFF444444), width = 72)
                val lockText = if (isConnected) when (timecode.lockState) {
                    0 -> "FREE"
                    1 -> if (isLtcDevice) "LTC" else "HDMI"
                    2 -> "RTC"
                    3 -> "BLE"
                    else -> "?"
                } else "---"
                OledBox(text = lockText, color = if (isConnected) Color(0xFFFFAA00) else Color(0xFF444444), width = 52)
                OledBox(text = if (isConnected) if (timecode.autoFps) "A" else "M" else "-", color = if (isConnected) Color(0xFFAA66FF) else Color(0xFF444444), width = 28)
                OledBox(text = if (isConnected && timecode.fps > 0) "${timecode.fps}fps" else "---", color = if (isConnected) Color(0xFF88CCFF) else Color(0xFF444444), width = 56)
                OledBox(text = if (isConnected) "LTC-${timecode.ltcModeText}" else "---", color = if (isConnected) Color(0xFF66DDFF) else Color(0xFF444444), width = 56)
            }
        }
    }
}

@Composable
private fun OledBox(text: String, color: Color, width: Int = 40) {
    Surface(
        shape = RoundedCornerShape(4.dp),
        border = BorderStroke(1.dp, color),
        color = Color.Transparent,
        modifier = Modifier.width(width.dp).height(28.dp),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = text,
                color = color,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigDrawer(
    bleManager: BleManager,
    scannedDevices: List<BleDevice>,
    connectionState: ConnectionState,
    onClose: () -> Unit,
    onStatus: (String) -> Unit,
    onShowJam: () -> Unit,
    currentTimecode: Timecode,
) {
    val connectedName by bleManager.deviceName.collectAsStateWithLifecycle()
    val deviceState by bleManager.deviceState.collectAsStateWithLifecycle()
    val isClap = connectedName.contains("CLAP", ignoreCase = true)
    val isLtc = connectedName.contains("LTC", ignoreCase = true)
    var selectedFps by remember { mutableStateOf(25) }
    var dropFrame by remember { mutableStateOf(false) }
    var brightness by remember { mutableStateOf(7) }
    var deviceName by remember { mutableStateOf(connectedName) }
    LaunchedEffect(connectedName) { deviceName = connectedName }
    LaunchedEffect(connectionState) {
        if (connectionState == ConnectionState.CONNECTED) {
            bleManager.readState()
        } else if (connectionState == ConnectionState.DISCONNECTED) {
            selectedFps = 25
            dropFrame = false
            brightness = 7
            deviceName = connectedName
        }
    }

    Column(
        modifier = Modifier
            .width(320.dp)
            .padding(16.dp)
            .verticalScroll(rememberScrollState())
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Configuration", style = MaterialTheme.typography.headlineSmall)
            Text(
                "✕",
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.clickable { onClose() }
            )
        }

        // BLE section
        Text("BLE Connection", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
        Spacer(Modifier.height(8.dp))

        if (connectionState == ConnectionState.DISCONNECTED || connectionState == ConnectionState.SCANNING) {
            Button(
                onClick = { bleManager.startScan() },
                enabled = connectionState != ConnectionState.SCANNING,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(if (connectionState == ConnectionState.SCANNING) "Scanning..." else "Scan for Devices")
            }
        } else {
            Button(
                onClick = { bleManager.disconnect() },
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFB00020)),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Disconnect")
            }
        }

        if (scannedDevices.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text("Devices:", style = MaterialTheme.typography.bodySmall)
            scannedDevices.forEach { device ->
                Surface(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 2.dp)
                        .clickable { bleManager.connect(device.address, device.name) },
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                ) {
                    Text(
                        "${device.name}\n${device.address}",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(12.dp)
                    )
                }
            }
        }

        // Show config items only when connected
        if (connectionState == ConnectionState.CONNECTED) {
        Divider(modifier = Modifier.padding(vertical = 16.dp))

        // FPS
        Text("Frame Rate", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
        Spacer(Modifier.height(4.dp))
        val autoFps = currentTimecode.autoFps
        val deviceFps = currentTimecode.fps
        LaunchedEffect(deviceFps) {
            if (!autoFps && deviceFps in listOf(24, 25, 30, 50, 60)) {
                selectedFps = deviceFps
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf(24, 25, 30, 50, 60).forEach { fps ->
                FilterChip(
                    selected = if (autoFps) deviceFps == fps else selectedFps == fps,
                    onClick = {
                        selectedFps = fps
                        bleManager.sendConfig("fps", fps.toString())
                        onStatus("FPS set to $fps")
                    },
                    label = { Text(if (autoFps && deviceFps == fps) "$fps A" else "$fps") }
                )
            }
        }

        Spacer(Modifier.height(12.dp))

        // Drop frame
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Drop Frame", modifier = Modifier.weight(1f))
            Switch(
                checked = dropFrame,
                onCheckedChange = {
                    dropFrame = it
                    bleManager.sendConfig("df", if (it) "1" else "0")
                    onStatus("Drop frame ${if (it) "on" else "off"}")
                }
            )
        }

        Spacer(Modifier.height(12.dp))

        // Jam timecode
        Button(onClick = onShowJam, modifier = Modifier.fillMaxWidth()) {
            Text("Jam Timecode")
        }

        Divider(modifier = Modifier.padding(vertical = 16.dp))

        if (isClap) {
            // Brightness — only on CLAP devices with LED matrix
            Text("Brightness: $brightness", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
            Slider(
                value = brightness.toFloat(),
                onValueChange = { brightness = it.toInt() },
                onValueChangeFinished = {
                    bleManager.sendConfig("brightness", brightness.toString())
                },
                valueRange = 0f..15f,
                steps = 14,
            )
            Row(horizontalArrangement = Arrangement.SpaceBetween) {
                Text("0", style = MaterialTheme.typography.bodySmall)
                Text("15", style = MaterialTheme.typography.bodySmall)
            }
            Divider(modifier = Modifier.padding(vertical = 16.dp))
        }

        if (isLtc) {
            // Master/Slave mode switch — only on LTC devices
            Text("Device Mode", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = true,
                    onClick = {
                        bleManager.sendConfig("mode", "slave")
                        onStatus("Switching to slave mode...")
                    },
                    label = { Text("Slave") }
                )
                FilterChip(
                    selected = false,
                    onClick = {
                        bleManager.sendConfig("mode", "master")
                        onStatus("Switching to master mode...")
                    },
                    label = { Text("Master") }
                )
            }
            Spacer(Modifier.height(4.dp))
            Text("Changes mode and restarts", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
            Divider(modifier = Modifier.padding(vertical = 16.dp))
        }

            // Master connection — scan, select, disconnect
            Text("Master", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
            Spacer(Modifier.height(8.dp))

            val isConnected = deviceState["conn"] == "1"
            val connName = deviceState["conn_name"] ?: ""
            val isScanning = deviceState["scanning"] == "1"
            val scanCount = deviceState["scan_count"]?.toIntOrNull() ?: 0

            // Poll device state while scan is in progress
            LaunchedEffect(isScanning) {
                if (isScanning) {
                    while (true) {
                        delay(1000)
                        bleManager.readState()
                        val s = bleManager.deviceState.value["scanning"]
                        if (s != "1") break
                    }
                }
            }

            if (isConnected) {
                Text("Connected to: $connName", color = Color(0xFF4CAF50), style = MaterialTheme.typography.bodySmall)
                Spacer(Modifier.height(8.dp))
                Button(
                    onClick = { bleManager.sendConfig("disconnect_master", "") },
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFB00020)),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Disconnect from Master")
                }
            } else {
                Button(
                    onClick = { bleManager.sendConfig("scan", "") },
                    enabled = !isScanning,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text(if (isScanning) "Scanning..." else "Scan for Masters")
                }

                if (scanCount > 0) {
                    Spacer(Modifier.height(8.dp))
                    Text("Devices:", style = MaterialTheme.typography.bodySmall)
                    for (i in 0 until scanCount) {
                        val name = deviceState["scan_name_$i"] ?: ""
                        val addr = deviceState["scan_addr_$i"] ?: ""
                        if (addr.isNotEmpty()) {
                            Surface(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 2.dp)
                                    .clickable {
                                        bleManager.sendConfig("select", addr)
                                        onStatus("Connecting to $name...")
                                    },
                                shape = RoundedCornerShape(8.dp),
                                color = MaterialTheme.colorScheme.surfaceVariant,
                            ) {
                                Text(
                                    "${name.ifEmpty { addr }}\n$addr",
                                    style = MaterialTheme.typography.bodySmall,
                                    modifier = Modifier.padding(12.dp)
                                )
                            }
                        }
                    }
                }
            }
            Divider(modifier = Modifier.padding(vertical = 16.dp))

        // Device name
        Text("Device Name", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
        Spacer(Modifier.height(4.dp))
        OutlinedTextField(
            value = deviceName,
            onValueChange = { deviceName = it },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        Button(
            onClick = {
                bleManager.sendConfig("name", deviceName)
                onStatus("Name set")
            },
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Set Name")
        }

        Divider(modifier = Modifier.padding(vertical = 16.dp))

        // WiFi
        Text("WiFi", style = MaterialTheme.typography.titleSmall, color = Color.Gray)
        Spacer(Modifier.height(8.dp))

        val context = LocalContext.current
        val prefs = remember { context.getSharedPreferences("tcwl", Context.MODE_PRIVATE) }

        // Use SharedPreferences as initial fallback; update from device state when BLE read completes
        var wifiEnabled by remember { mutableStateOf(prefs.getBoolean("wifi_en", true)) }
        var wifiSsid by remember { mutableStateOf(prefs.getString("wifi_ssid", "") ?: "") }
        var wifiPass by remember { mutableStateOf(prefs.getString("wifi_pass", "") ?: "") }
        var wifiShowPass by remember { mutableStateOf(false) }

        // Override with real device state once the BLE read arrives
        LaunchedEffect(deviceState) {
            if (deviceState.isNotEmpty()) {
                deviceState["wifi"]?.let { wifiEnabled = it == "1" }
                deviceState["ssid"]?.takeIf { it.isNotEmpty() && it != "(null)" }
                    ?.let { wifiSsid = it }
            }
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("WiFi Radio", modifier = Modifier.weight(1f))
            Switch(
                checked = wifiEnabled,
                onCheckedChange = {
                    wifiEnabled = it
                    prefs.edit().putBoolean("wifi_en", it).apply()
                    bleManager.sendConfig("wifi", if (it) "1" else "0")
                    bleManager.readState()
                    onStatus("WiFi ${if (it) "enabled" else "disabled"}")
                }
            )
        }

        // Show IP when connected to WiFi
        val wifiIp = deviceState["ip"]?.takeIf { it.isNotEmpty() && it != "0.0.0.0" && it != "(null)" }
        if (wifiEnabled && wifiIp != null) {
            Spacer(Modifier.height(4.dp))
            Text("IP: $wifiIp", color = Color(0xFF4CAF50), style = MaterialTheme.typography.bodySmall)
        }

        Spacer(Modifier.height(8.dp))

        OutlinedTextField(
            value = wifiSsid,
            onValueChange = { wifiSsid = it },
            label = { Text("SSID") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(4.dp))
        OutlinedTextField(
            value = wifiPass,
            onValueChange = { wifiPass = it },
            label = { Text("Password") },
            singleLine = true,
            visualTransformation = if (wifiShowPass) VisualTransformation.None else PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            trailingIcon = {
                Text(
                    if (wifiShowPass) "🙈" else "👁",
                    modifier = Modifier.clickable { wifiShowPass = !wifiShowPass }
                )
            },
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = {
                    prefs.edit().putString("wifi_ssid", wifiSsid).putString("wifi_pass", wifiPass).apply()
                    bleManager.sendConfig("wifi_ssid", "$wifiSsid,$wifiPass")
                    bleManager.readState()
                    onStatus("WiFi connecting...")
                },
                enabled = wifiSsid.isNotBlank(),
                modifier = Modifier.weight(1f)
            ) {
                Text("Connect")
            }
            OutlinedButton(
                onClick = {
                    bleManager.sendConfig("wifi_forget", "1")
                    bleManager.readState()
                    wifiSsid = ""
                    wifiPass = ""
                    prefs.edit().remove("wifi_ssid").remove("wifi_pass").apply()
                    onStatus("WiFi credentials cleared")
                },
                modifier = Modifier.weight(1f)
            ) {
                Text("Forget")
            }
        }

        Divider(modifier = Modifier.padding(vertical = 16.dp))

        Button(
            onClick = {
                bleManager.sendConfig("restart", "")
                onStatus("Device restarting")
            },
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFB00020)),
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Restart Device")
        }

        }

        if (connectionState == ConnectionState.CONNECTED) {
            val fwVer = deviceState["fw"] ?: ""
            if (fwVer.isNotEmpty()) {
                Text(
                    "FW $fwVer",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.Gray,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center,
                )
            }
        }

        val fwVer = deviceState["fw"] ?: ""
        Text(
            "App ${com.tcwl.timecode.BuildConfig.FW_VERSION}",
            style = MaterialTheme.typography.bodySmall,
            color = Color.Gray,
            modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
            textAlign = TextAlign.Center,
        )
        if (fwVer.isNotEmpty()) {
            Text(
                "FW $fwVer",
                style = MaterialTheme.typography.bodySmall,
                color = Color.Gray,
                modifier = Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center,
            )
        }

        Spacer(Modifier.height(16.dp))
    }
}

@Composable
fun JamDialog(
    current: Timecode,
    onDismiss: () -> Unit,
    onJam: (dd: Int, hh: Int, mm: Int, ss: Int, ff: Int) -> Unit,
) {
    var dd by remember { mutableStateOf(current.dd.toString().padStart(2, '0')) }
    var hh by remember { mutableStateOf(current.hh.toString().padStart(2, '0')) }
    var mm by remember { mutableStateOf(current.mm.toString().padStart(2, '0')) }
    var ss by remember { mutableStateOf(current.ss.toString().padStart(2, '0')) }
    var ff by remember { mutableStateOf(current.ff.toString().padStart(2, '0')) }

    fun digitOnly(s: String, max: Int): String {
        val digits = s.filter { it.isDigit() }.take(2)
        val num = digits.toIntOrNull() ?: return digits
        return if (num > max) max.toString().padStart(2, '0') else digits
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Set Timecode") },
        text = {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Row(
                    horizontalArrangement = Arrangement.Center,
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TcField("DD", dd) { dd = digitOnly(it, 99) }
                    Text(":", modifier = Modifier.padding(horizontal = 2.dp))
                    TcField("HH", hh) { hh = digitOnly(it, 23) }
                    Text(":", modifier = Modifier.padding(horizontal = 2.dp))
                    TcField("MM", mm) { mm = digitOnly(it, 59) }
                    Text(":", modifier = Modifier.padding(horizontal = 2.dp))
                    TcField("SS", ss) { ss = digitOnly(it, 59) }
                    Text(".", modifier = Modifier.padding(horizontal = 2.dp))
                    TcField("FF", ff) { ff = digitOnly(it, 59) }
                }
                Spacer(Modifier.height(8.dp))
                Text(
                    "Current: ${current.displayWithDays}",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.Gray
                )
            }
        },
        confirmButton = {
            TextButton(onClick = {
                onJam(
                    dd.toIntOrNull() ?: current.dd,
                    hh.toIntOrNull() ?: current.hh,
                    mm.toIntOrNull() ?: current.mm,
                    ss.toIntOrNull() ?: current.ss,
                    ff.toIntOrNull() ?: current.ff,
                )
            }) { Text("Jam") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun TcField(label: String, value: String, onValueChange: (String) -> Unit) {
    var textFieldValue by remember { mutableStateOf(TextFieldValue(value)) }
    var isFocused by remember { mutableStateOf(false) }

    LaunchedEffect(value) {
        if (textFieldValue.text != value) {
            textFieldValue = TextFieldValue(value)
        }
    }

    LaunchedEffect(isFocused) {
        if (isFocused) {
            withFrameMillis { }
            textFieldValue = textFieldValue.copy(
                selection = TextRange(0, textFieldValue.text.length)
            )
        }
    }

    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, style = MaterialTheme.typography.bodySmall)
        OutlinedTextField(
            value = textFieldValue,
            onValueChange = { newValue: TextFieldValue ->
                textFieldValue = newValue
                onValueChange(newValue.text)
            },
            modifier = Modifier
                .width(56.dp)
                .onFocusChanged { state ->
                    isFocused = state.isFocused
                },
            singleLine = true,
            textStyle = MaterialTheme.typography.bodyLarge.copy(
                fontFamily = FontFamily.Monospace,
                textAlign = TextAlign.Center
            ),
        )
    }
}
