package cz.windmi.control.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import cz.windmi.control.ui.theme.Bg
import cz.windmi.control.ui.theme.Border
import cz.windmi.control.ui.theme.BtnRoundBorder
import cz.windmi.control.ui.theme.Ekv
import cz.windmi.control.ui.theme.Fault
import cz.windmi.control.ui.theme.FaultText
import cz.windmi.control.ui.theme.Muted
import cz.windmi.control.ui.theme.Ok
import cz.windmi.control.ui.theme.Panel
import cz.windmi.control.ui.theme.Room
import cz.windmi.control.ui.theme.TextPrimary
import cz.windmi.control.ui.theme.Warn
import cz.windmi.control.ui.theme.Water

@Composable
fun WindmiApp(viewModel: WindmiViewModel) {
    val state by viewModel.ui.collectAsState()
    val lifecycleOwner = LocalLifecycleOwner.current

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> viewModel.onForeground(true)
                Lifecycle.Event.ON_PAUSE -> viewModel.onForeground(false)
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Bg)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        TopBar(state)
        if (state.faultVisible && state.faultText.isNotBlank()) {
            FaultBanner(state.faultText)
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (state.selectedTab) {
                0 -> HomeScreen(state, viewModel)
                else -> SettingsScreen(state, viewModel)
            }
        }
        BottomNav(selected = state.selectedTab, onSelect = viewModel::selectTab)
    }
}

@Composable
private fun TopBar(state: AppUiState) {
    val mqtt = state.mqttSig()
    val lin = state.linSig()
    val mqttLabel = state.flashMessage ?: mqtt.second
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Windmi", color = TextPrimary, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SigChip(label = "MQTT", meta = mqtt.first to mqttLabel)
            SigChip(label = "MB", meta = lin)
        }
    }
    HorizontalDivider(color = Border)
}

@Composable
private fun SigChip(meta: Pair<SigState, String>, label: String) {
    val color = when (meta.first) {
        SigState.Ok -> Ok
        SigState.Warn -> Warn
        SigState.Error -> Fault
        SigState.Off -> Muted
    }
    Box(
        Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Panel)
            .border(1.dp, Border, RoundedCornerShape(8.dp))
            .padding(horizontal = 8.dp, vertical = 4.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = color, fontSize = 12.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun FaultBanner(text: String) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Fault.copy(alpha = 0.18f))
            .padding(12.dp),
    ) {
        Text("Poruchové hlášení", color = FaultText, fontWeight = FontWeight.Bold)
        Text(text, color = FaultText)
    }
}

@Composable
private fun HomeScreen(state: AppUiState, vm: WindmiViewModel) {
    val titleColor = when (state.regMode) {
        RegMode.Room -> Room
        RegMode.Equitherm -> Ekv
        RegMode.Water -> Water
    }
    val title = when (state.regMode) {
        RegMode.Room -> "Nastavení pokojové teploty"
        RegMode.Equitherm -> "Ekvitermní SP vody"
        RegMode.Water -> "Nastavení teploty vody"
    }
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(title, color = titleColor, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(12.dp))
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            RoundBtn("−") { vm.adjustSetpoint(-1) }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        state.displaySetpoint(),
                        color = TextPrimary,
                        fontSize = 56.sp,
                        fontWeight = FontWeight.Light,
                    )
                    Text(
                        text = " °C",
                        color = Muted,
                        fontSize = 20.sp,
                        modifier = Modifier.padding(bottom = 10.dp),
                    )
                }
                if (state.regMode == RegMode.Equitherm && state.mqttConnected && state.eqOffset != null) {
                    Text(
                        "Korekce ${formatOffset(state.eqOffset)} °C",
                        color = Ekv,
                        fontSize = 13.sp,
                    )
                }
            }
            RoundBtn("+") { vm.adjustSetpoint(1) }
        }
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            ModeChip("Pokoj", state.regMode == RegMode.Room, Room) { vm.setRegMode(RegMode.Room) }
            ModeChip("Ekviterm", state.regMode == RegMode.Equitherm, Ekv) {
                vm.setRegMode(RegMode.Equitherm)
            }
            ModeChip("Ruční", state.regMode == RegMode.Water, Water) { vm.setRegMode(RegMode.Water) }
        }
        Spacer(Modifier.height(20.dp))
        TempGrid(state)
        Spacer(Modifier.height(16.dp))
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            PowerBtn("START", state.power, Ok, Modifier.weight(1f)) { vm.setPower(true) }
            PowerBtn(
                "STOP",
                !state.power && state.mqttConnected,
                Fault,
                Modifier.weight(1f),
            ) { vm.setPower(false) }
        }
        Spacer(Modifier.height(18.dp))
        StatusRow(state)
    }
}

@Composable
private fun RoundBtn(label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .size(56.dp)
            .clip(CircleShape)
            .background(Panel)
            .border(1.dp, BtnRoundBorder, CircleShape)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = TextPrimary, fontSize = 28.sp)
    }
}

@Composable
private fun ModeChip(label: String, active: Boolean, accent: Color, onClick: () -> Unit) {
    val bg = if (active) accent.copy(alpha = 0.18f) else Panel
    val borderColor = if (active) accent else Border
    val fg = if (active) accent else Muted
    Text(
        text = label,
        color = fg,
        fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal,
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(bg)
            .border(1.dp, borderColor, RoundedCornerShape(20.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 8.dp),
    )
}

@Composable
private fun TempGrid(state: AppUiState) {
    val live = state.mqttConnected
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            TempCard(
                "Vnitřní teplota",
                formatTemp(if (live) state.temps.room else null, 1),
                Modifier.weight(1f),
            )
            TempCard(
                "Venkovní teplota",
                formatTemp(if (live) state.temps.outdoor else null, 1),
                Modifier.weight(1f),
                Water,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            TempCard(
                "Teplota na vstupu",
                formatTemp(if (live) state.temps.inlet else null, 0),
                Modifier.weight(1f),
            )
            TempCard(
                "Teplota na výstupu",
                formatTemp(if (live) state.temps.outlet else null, 0),
                Modifier.weight(1f),
                Room,
            )
        }
    }
}

@Composable
private fun TempCard(
    label: String,
    value: String,
    mod: Modifier = Modifier,
    valueColor: Color = TextPrimary,
) {
    Column(
        mod
            .clip(RoundedCornerShape(12.dp))
            .background(Panel)
            .border(1.dp, Border, RoundedCornerShape(12.dp))
            .padding(12.dp),
    ) {
        Text(label, color = Muted, fontSize = 12.sp)
        Text(value, color = valueColor, fontSize = 28.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun PowerBtn(
    label: String,
    active: Boolean,
    color: Color,
    mod: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        modifier = mod.height(48.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (active) color.copy(alpha = 0.25f) else Panel,
            contentColor = if (active) color else TextPrimary,
        ),
        shape = RoundedCornerShape(12.dp),
    ) {
        Text(label, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun StatusRow(state: AppUiState) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceEvenly,
    ) {
        StatText("⏻", state.power, warn = false)
        StatText("🔄", state.pump, warn = false)
        StatText("🌀", state.compressor, warn = false)
        StatText("❄", state.defrost, warn = true)
        StatText("⚡", state.elec, warn = true)
    }
}

@Composable
private fun StatText(glyph: String, on: Boolean, warn: Boolean) {
    val color = when {
        on && warn -> Warn
        on -> Ok
        else -> Muted.copy(alpha = 0.45f)
    }
    Text(glyph, color = color, fontSize = 22.sp)
}

@Composable
private fun SettingsScreen(state: AppUiState, vm: WindmiViewModel) {
    val fieldColors = OutlinedTextFieldDefaults.colors(
        focusedTextColor = TextPrimary,
        unfocusedTextColor = TextPrimary,
        focusedBorderColor = Room,
        unfocusedBorderColor = Border,
        focusedLabelColor = Muted,
        unfocusedLabelColor = Muted,
        cursorColor = Room,
    )
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
    ) {
        Text(
            "Připojení MQTT",
            color = TextPrimary,
            fontSize = 20.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(6.dp))
        Text(
            "Aplikace slouží jen k ovládání. Časový plán a regulátor PID zůstávají na Tab5.",
            color = Muted,
            fontSize = 13.sp,
        )
        Spacer(Modifier.height(16.dp))
        OutlinedTextField(
            value = state.brokerUrl,
            onValueChange = { vm.updateDraft(host = it) },
            modifier = Modifier.fillMaxWidth(),
            label = { Text("Broker (WSS)") },
            singleLine = true,
            colors = fieldColors,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = state.brokerUser,
            onValueChange = { vm.updateDraft(user = it) },
            modifier = Modifier.fillMaxWidth(),
            label = { Text("Uživatel") },
            singleLine = true,
            colors = fieldColors,
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = state.brokerPassword,
            onValueChange = { vm.updateDraft(password = it) },
            modifier = Modifier.fillMaxWidth(),
            label = { Text("Heslo") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            colors = fieldColors,
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = state.topicPrefix,
            onValueChange = { vm.updateDraft(prefix = it) },
            modifier = Modifier.fillMaxWidth(),
            label = { Text("Topic prefix") },
            singleLine = true,
            colors = fieldColors,
        )
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Button(
                onClick = vm::connect,
                enabled = !state.sessionActive,
                modifier = Modifier.weight(1f),
            ) { Text("Připojit") }
            OutlinedButton(
                onClick = vm::disconnect,
                enabled = state.sessionActive,
                modifier = Modifier.weight(1f),
            ) { Text("Odpojit") }
        }
        Spacer(Modifier.height(12.dp))
        val statusColor = when {
            state.mqttStatus == MqttLinkStatus.Error -> Fault
            state.mqttConnected -> Ok
            else -> Muted
        }
        Text(state.settingsStatus, color = statusColor, textAlign = TextAlign.Start)
    }
}

@Composable
private fun BottomNav(selected: Int, onSelect: (Int) -> Unit) {
    HorizontalDivider(color = Border)
    Row(
        Modifier
            .fillMaxWidth()
            .background(Panel)
            .padding(vertical = 8.dp),
        horizontalArrangement = Arrangement.SpaceEvenly,
    ) {
        NavItem("Domů", Icons.Filled.Home, selected == 0) { onSelect(0) }
        NavItem("Nastavení", Icons.Filled.Settings, selected == 1) { onSelect(1) }
    }
}

@Composable
private fun NavItem(label: String, icon: ImageVector, active: Boolean, onClick: () -> Unit) {
    val color = if (active) Room else Muted
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .width(120.dp)
            .clickable(onClick = onClick)
            .padding(6.dp),
    ) {
        Icon(icon, contentDescription = label, tint = color)
        Text(label, color = color, fontSize = 12.sp)
    }
}
