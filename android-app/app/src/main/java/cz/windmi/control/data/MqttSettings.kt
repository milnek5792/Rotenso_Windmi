package cz.windmi.control.data

import java.net.URI

data class MqttSettings(
    val host: String = DEFAULT_BROKER,
    val user: String = "",
    val password: String = "",
    val prefix: String = "windmi",
    val autoConnect: Boolean = true,
) {
    companion object {
        const val DEFAULT_HOST = "n9e16b3c.ala.eu-central-1.emqxsl.com"

        /** Nativní MQTTS — stejný broker/port jako Tab5 (ne WSS z PWA). */
        const val DEFAULT_BROKER = "mqtts://$DEFAULT_HOST:8883"

        @Deprecated("PWA-only; Android používá DEFAULT_BROKER (mqtts:8883)")
        const val DEFAULT_WSS = "wss://$DEFAULT_HOST:8084/mqtt"

        /**
         * PWA potřebuje wss://host:8084/mqtt (WebSocket v prohlížeči).
         * Android/Tab5 používají mqtts://host:8883 (MQTT přes TLS).
         */
        fun normalizeBrokerUrl(raw: String): String {
            val t = raw.trim()
            if (t.isEmpty()) return DEFAULT_BROKER
            if (!t.contains("://")) {
                val hostPort = t.substringBefore('/').trim()
                if (hostPort.isEmpty()) return DEFAULT_BROKER
                return if (hostPort.contains(':')) {
                    "mqtts://$hostPort"
                } else {
                    "mqtts://$hostPort:8883"
                }
            }
            return try {
                val uri = URI(t)
                val scheme = (uri.scheme ?: "").lowercase()
                val host = uri.host ?: return DEFAULT_BROKER
                when (scheme) {
                    "ws", "wss" -> "mqtts://$host:8883"
                    "mqtts", "ssl" -> {
                        val port = if (uri.port > 0) uri.port else 8883
                        "mqtts://$host:$port"
                    }
                    "mqtt", "tcp" -> {
                        val port = if (uri.port > 0) uri.port else 1883
                        "mqtt://$host:$port"
                    }
                    else -> t
                }
            } catch (_: Exception) {
                DEFAULT_BROKER
            }
        }
    }
}
