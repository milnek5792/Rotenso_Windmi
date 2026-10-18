package cz.windmi.control.screen

enum class RegMode { Room, Equitherm, Water }

enum class MqttLinkStatus { Idle, Connecting, Connected, Error }

enum class SigState { Off, Ok, Warn, Error }

data class Temps(
    val room: Float? = null,
    val outdoor: Float? = null,
    val inlet: Float? = null,
    val outlet: Float? = null,
)

data class TelePatch(
    val room: Float? = null,
    val outdoor: Float? = null,
    val inlet: Float? = null,
    val outlet: Float? = null,
    val setpoint: Float? = null,
    val clearSetpoint: Boolean = false,
    val regMode: RegMode? = null,
    val eqOffset: Float? = null,
    val power: Boolean? = null,
    val pump: Boolean? = null,
    val compressor: Boolean? = null,
    val lin: Boolean? = null,
    val defrost: Boolean? = null,
    val elec: Boolean? = null,
    val alarm: Boolean? = null,
    val poruchaText: String? = null,
    val watchActive: Boolean? = null,
    val tab5Online: Boolean? = null,
)

data class AppUiState(
    val mqttStatus: MqttLinkStatus = MqttLinkStatus.Idle,
    val mqttError: String = "",
    val mqttConnected: Boolean = false,
    val tab5Online: Boolean = false,
    val teleFresh: Boolean = false,
    val watchActive: Boolean = false,
    val regMode: RegMode = RegMode.Room,
    val setpoint: Float? = null,
    val eqOffset: Float? = null,
    val power: Boolean = false,
    val pump: Boolean = false,
    val compressor: Boolean = false,
    val defrost: Boolean = false,
    val elec: Boolean = false,
    val lin: Boolean = false,
    val alarm: Boolean = false,
    val poruchaText: String = "",
    val faultVisible: Boolean = false,
    val faultText: String = "",
    val temps: Temps = Temps(),
    val flashMessage: String? = null,
    val brokerUrl: String = "",
    val brokerUser: String = "",
    val brokerPassword: String = "",
    val topicPrefix: String = "windmi",
    val sessionActive: Boolean = false,
    val selectedTab: Int = 0,
) {
    val settingsStatus: String
        get() = when {
            mqttStatus == MqttLinkStatus.Error -> mqttError
            mqttConnected && tab5Online -> "Připojeno — Tab5 online"
            mqttConnected -> "Připojeno — čekám na windmi/availability"
            mqttStatus == MqttLinkStatus.Connecting -> mqttError.ifBlank { "Připojování…" }
            else -> ""
        }

    fun isLinOk(): Boolean {
        if (!mqttConnected || !tab5Online || !teleFresh) return false
        if (lin) return true
        return temps.inlet != null || temps.outdoor != null || pump || compressor
    }

    fun mqttSig(): Pair<SigState, String> = when {
        mqttStatus == MqttLinkStatus.Connecting -> SigState.Warn to "MQTT: připojování"
        mqttStatus == MqttLinkStatus.Error -> SigState.Error to "MQTT: ${mqttError.ifBlank { "chyba" }}"
        mqttConnected && tab5Online -> SigState.Ok to "MQTT: připojeno"
        mqttConnected -> SigState.Warn to "MQTT: čekám na Tab5"
        else -> SigState.Off to "MQTT: odpojeno"
    }

    fun linSig(): Pair<SigState, String> = when {
        !mqttConnected || mqttStatus == MqttLinkStatus.Connecting ->
            SigState.Off to "MB: odpojeno"
        !tab5Online -> SigState.Off to "MB: Tab5 offline"
        isLinOk() -> SigState.Ok to "MB: OK"
        teleFresh -> SigState.Warn to "MB: bez spojení"
        else -> SigState.Off to "MB: čekám na data"
    }

    fun displaySetpoint(): String {
        if (!mqttConnected) return "—"
        return when (regMode) {
            RegMode.Room -> formatTemp(setpoint, 1)
            RegMode.Equitherm, RegMode.Water -> formatTemp(setpoint, 0)
        }
    }
}

fun formatTemp(value: Float?, decimals: Int): String {
    if (value == null || value.isNaN()) return "—"
    return if (decimals == 0) {
        value.toInt().toString()
    } else {
        String.format("%.${decimals}f", value)
    }
}

fun formatOffset(value: Float?): String {
    if (value == null || value.isNaN()) return "—"
    val n = Math.round(value)
    return if (n > 0) "+$n" else n.toString()
}
