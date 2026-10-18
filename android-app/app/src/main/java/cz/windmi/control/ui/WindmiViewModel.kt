package cz.windmi.control.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import cz.windmi.control.data.MqttSettings
import cz.windmi.control.data.SettingsStore
import cz.windmi.control.mqtt.MqttEvent
import cz.windmi.control.mqtt.MqttRepository
import cz.windmi.control.mqtt.PublishResult
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class WindmiViewModel(
    private val settingsStore: SettingsStore,
    private val mqtt: MqttRepository,
) : ViewModel() {
    private val _ui = MutableStateFlow(AppUiState())
    val ui: StateFlow<AppUiState> = _ui.asStateFlow()

    private var lastTeleAt = 0L
    private var faultJob: Job? = null
    private var faultPending: Pair<Boolean, String>? = null
    private var flashJob: Job? = null
    private var autoStarted = false

    init {
        mqtt.setListener { event ->
            viewModelScope.launch { onMqttEvent(event) }
        }
        viewModelScope.launch {
            settingsStore.settings.collect { s ->
                _ui.update {
                    it.copy(
                        brokerUrl = s.host,
                        brokerUser = s.user,
                        brokerPassword = s.password,
                        topicPrefix = s.prefix,
                    )
                }
                if (!autoStarted && s.autoConnect && s.host.isNotBlank() &&
                    s.user.isNotBlank() && s.password.isNotBlank()
                ) {
                    autoStarted = true
                    mqtt.connect(s)
                }
            }
        }
        viewModelScope.launch {
            while (true) {
                delay(5_000)
                checkTeleStale()
            }
        }
    }

    fun selectTab(index: Int) {
        _ui.update { it.copy(selectedTab = index) }
    }

    fun updateDraft(host: String? = null, user: String? = null, password: String? = null, prefix: String? = null) {
        _ui.update {
            it.copy(
                brokerUrl = host ?: it.brokerUrl,
                brokerUser = user ?: it.brokerUser,
                brokerPassword = password ?: it.brokerPassword,
                topicPrefix = prefix ?: it.topicPrefix,
            )
        }
    }

    fun connect() {
        val state = _ui.value
        val host = state.brokerUrl.trim()
        if (host.isEmpty()) {
            _ui.update {
                it.copy(mqttStatus = MqttLinkStatus.Error, mqttError = "Zadej URL brokeru (WSS)")
            }
            return
        }
        val settings = MqttSettings(
            host = host,
            user = state.brokerUser.trim(),
            password = state.brokerPassword,
            prefix = SettingsStore.normalizePrefix(state.topicPrefix),
            autoConnect = true,
        )
        viewModelScope.launch {
            settingsStore.save(settings)
            mqtt.connect(settings)
            refreshSession()
        }
    }

    fun disconnect() {
        viewModelScope.launch {
            settingsStore.setAutoConnect(false)
            mqtt.disconnect(sendWatchOff = true)
            refreshSession()
        }
    }

    fun onForeground(visible: Boolean) {
        if (!mqtt.isConnected()) return
        mqtt.publishWatch(visible)
    }

    fun adjustSetpoint(delta: Int) {
        val sign = if (delta > 0) "+" else "-"
        val mode = _ui.value.regMode
        val label = if (mode == RegMode.Equitherm) "korekce $sign" else "setpoint $sign"
        if (!sendCmd("setpoint", sign, label)) return
        _ui.update { state ->
            when (state.regMode) {
                RegMode.Room -> state.copy(
                    setpoint = state.setpoint?.let { clampRoomSp(it + if (delta > 0) 0.5f else -0.5f) },
                )
                RegMode.Equitherm -> state.copy(
                    eqOffset = (state.eqOffset ?: 0f) + if (delta > 0) 1f else -1f,
                )
                RegMode.Water -> state.copy(
                    setpoint = state.setpoint?.let { Math.round(it + if (delta > 0) 1f else -1f).toFloat() },
                )
            }
        }
    }

    fun setRegMode(mode: RegMode) {
        val payload = when (mode) {
            RegMode.Room -> "room"
            RegMode.Equitherm -> "equitherm"
            RegMode.Water -> "water"
        }
        if (!sendCmd("mode", payload, "mode $payload")) return
        _ui.update { it.copy(regMode = mode) }
    }

    fun setPower(on: Boolean) {
        if (!sendCmd("power", if (on) "ON" else "OFF", if (on) "START" else "STOP")) return
        _ui.update {
            it.copy(
                power = on,
                pump = if (on) it.pump else false,
                compressor = if (on) it.compressor else false,
            )
        }
    }

    private fun sendCmd(suffix: String, payload: String, okLabel: String): Boolean {
        if (!(mqtt.isConnected() || mqtt.sessionActive())) {
            flash("Nejdřív připoj MQTT v Nastavení")
            return false
        }
        return when (mqtt.publishCmd(suffix, payload)) {
            PublishResult.Fail -> {
                flash("MQTT neodpovídá — znovu Připojit")
                false
            }
            PublishResult.Queued -> {
                flash("… $okLabel")
                true
            }
            PublishResult.Ok -> {
                flash("→ $okLabel")
                true
            }
        }
    }

    private fun flash(msg: String) {
        flashJob?.cancel()
        _ui.update { it.copy(flashMessage = msg) }
        flashJob = viewModelScope.launch {
            delay(1200)
            _ui.update { it.copy(flashMessage = null) }
        }
    }

    private fun onMqttEvent(event: MqttEvent) {
        when (event) {
            is MqttEvent.Status -> {
                _ui.update {
                    it.copy(
                        mqttStatus = event.status,
                        mqttError = event.error,
                        mqttConnected = event.status == MqttLinkStatus.Connected,
                        sessionActive = mqtt.sessionActive(),
                    )
                }
                if (event.status == MqttLinkStatus.Connecting ||
                    event.status == MqttLinkStatus.Idle ||
                    event.status == MqttLinkStatus.Error
                ) {
                    clearLiveSignals(resetTab = true)
                }
            }
            is MqttEvent.Connection -> {
                _ui.update {
                    it.copy(
                        mqttConnected = event.connected,
                        sessionActive = mqtt.sessionActive(),
                    )
                }
                if (!event.connected) {
                    if (event.reconnecting) {
                        clearLiveSignals(resetTab = true)
                    } else {
                        resetTelemetry()
                    }
                }
            }
            is MqttEvent.Tele -> applyTele(event.patch)
        }
        refreshSession()
    }

    private fun applyTele(p: TelePatch) {
        lastTeleAt = System.currentTimeMillis()
        _ui.update { state ->
            var next = state.copy(teleFresh = true)
            if (p.room != null || p.outdoor != null || p.inlet != null || p.outlet != null) {
                next = next.copy(
                    temps = next.temps.copy(
                        room = p.room ?: next.temps.room,
                        outdoor = p.outdoor ?: next.temps.outdoor,
                        inlet = p.inlet ?: next.temps.inlet,
                        outlet = p.outlet ?: next.temps.outlet,
                    ),
                )
            }
            if (p.clearSetpoint) next = next.copy(setpoint = null)
            else if (p.setpoint != null) next = next.copy(setpoint = p.setpoint)
            if (p.regMode != null) next = next.copy(regMode = p.regMode)
            if (p.eqOffset != null) next = next.copy(eqOffset = p.eqOffset)
            if (p.power != null) next = next.copy(power = p.power)
            if (p.pump != null) next = next.copy(pump = p.pump)
            if (p.compressor != null) next = next.copy(compressor = p.compressor)
            if (p.lin != null) next = next.copy(lin = p.lin)
            if (p.defrost != null) next = next.copy(defrost = p.defrost)
            if (p.elec != null) next = next.copy(elec = p.elec)
            if (p.alarm != null) next = next.copy(alarm = p.alarm)
            if (p.poruchaText != null) next = next.copy(poruchaText = p.poruchaText)
            if (p.watchActive != null) next = next.copy(watchActive = p.watchActive)
            if (p.tab5Online != null) {
                next = next.copy(tab5Online = p.tab5Online)
                if (!p.tab5Online) {
                    return@update clearLiveOn(next)
                }
            }
            next
        }
        scheduleFaultBanner()
    }

    private fun scheduleFaultBanner() {
        val state = _ui.value
        if (!state.tab5Online || !state.teleFresh) {
            _ui.update { it.copy(faultVisible = false, faultText = "") }
            return
        }
        val text = state.poruchaText.trim()
        val wantShow = text.isNotEmpty() || state.alarm
        val nextText = text.ifEmpty { if (state.alarm) "Alarm hlášen přes MQTT" else "" }
        if (wantShow && state.faultVisible && state.faultText != nextText && nextText.isNotEmpty()) {
            _ui.update { it.copy(faultText = nextText) }
            return
        }
        if (faultPending == wantShow to nextText) return
        faultPending = wantShow to nextText
        faultJob?.cancel()
        faultJob = viewModelScope.launch {
            delay(if (wantShow) 2000 else 800)
            faultPending = null
            val s = _ui.value
            if (!s.tab5Online || !s.teleFresh) {
                _ui.update { it.copy(faultVisible = false, faultText = "") }
            } else {
                _ui.update {
                    it.copy(
                        faultVisible = wantShow,
                        faultText = if (wantShow) nextText else "",
                    )
                }
            }
        }
    }

    private fun checkTeleStale() {
        val state = _ui.value
        if (!state.mqttConnected || state.mqttStatus != MqttLinkStatus.Connected) return
        if (!state.tab5Online && !state.teleFresh) return
        if (lastTeleAt == 0L || System.currentTimeMillis() - lastTeleAt < TELE_STALE_MS) return
        clearLiveSignals(resetTab = true)
    }

    private fun clearLiveSignals(resetTab: Boolean) {
        _ui.update { clearLiveOn(it, resetTab) }
        lastTeleAt = 0L
        faultJob?.cancel()
        faultPending = null
    }

    private fun clearLiveOn(state: AppUiState, resetTab: Boolean = false): AppUiState {
        return state.copy(
            tab5Online = if (resetTab) false else state.tab5Online,
            teleFresh = false,
            watchActive = false,
            lin = false,
            power = false,
            pump = false,
            compressor = false,
            defrost = false,
            elec = false,
            setpoint = null,
            eqOffset = null,
            temps = Temps(),
            poruchaText = "",
            alarm = false,
            faultVisible = false,
            faultText = "",
        )
    }

    private fun resetTelemetry() {
        _ui.update {
            it.copy(
                tab5Online = false,
                teleFresh = false,
                watchActive = false,
                regMode = RegMode.Room,
                setpoint = null,
                eqOffset = null,
                power = false,
                pump = false,
                compressor = false,
                defrost = false,
                elec = false,
                lin = false,
                alarm = false,
                poruchaText = "",
                faultVisible = false,
                faultText = "",
                temps = Temps(),
            )
        }
        lastTeleAt = 0L
        faultJob?.cancel()
        faultPending = null
    }

    private fun refreshSession() {
        _ui.update {
            it.copy(
                sessionActive = mqtt.sessionActive(),
                mqttConnected = mqtt.isConnected() || it.mqttStatus == MqttLinkStatus.Connected,
            )
        }
    }

    private fun clampRoomSp(v: Float): Float {
        val stepped = Math.round(v * 2f) / 2f
        return stepped.coerceIn(16f, 24f)
    }

    override fun onCleared() {
        mqtt.setListener(null)
        mqtt.disconnect(sendWatchOff = true)
        super.onCleared()
    }

    companion object {
        private const val TELE_STALE_MS = 45_000L
    }
}
