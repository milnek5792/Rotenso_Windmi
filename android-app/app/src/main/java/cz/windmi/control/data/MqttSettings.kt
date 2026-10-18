package cz.windmi.control.data

data class MqttSettings(
    val host: String = DEFAULT_WSS,
    val user: String = "",
    val password: String = "",
    val prefix: String = "windmi",
    val autoConnect: Boolean = true,
) {
    companion object {
        const val DEFAULT_WSS =
            "wss://n9e16b3c.ala.eu-central-1.emqxsl.com:8084/mqtt"
    }
}
