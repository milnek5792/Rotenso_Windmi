package cz.windmi.control.mqtt

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import cz.windmi.control.data.MqttSettings
import cz.windmi.control.data.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicLong

/**
 * MQTT session žije s procesem aplikace — ne s Activity/ViewModel.
 * Po delší pauze na pozadí Android/NAT často nechá mrtvý socket; při návratu
 * proto vynutíme nový connect (HiveMQ auto-reconnect sám nestačí).
 */
class MqttSessionController(
    private val settingsStore: SettingsStore,
    private val mqtt: MqttRepository,
    private val scope: CoroutineScope,
) {
    @Volatile
    private var latest: MqttSettings? = null

    private val backgroundedAtMs = AtomicLong(0)

    private val lifecycle = object : DefaultLifecycleObserver {
        override fun onStart(owner: LifecycleOwner) {
            scope.launch { ensureConnected(fromForeground = true) }
        }

        override fun onStop(owner: LifecycleOwner) {
            backgroundedAtMs.set(System.currentTimeMillis())
            if (mqtt.isConnected()) {
                mqtt.publishWatch(false)
            }
        }
    }

    fun start() {
        ProcessLifecycleOwner.get().lifecycle.addObserver(lifecycle)
        scope.launch {
            settingsStore.settings.collectLatest { s ->
                latest = s
                if (!s.autoConnect) {
                    return@collectLatest
                }
                ensureConnected(fromForeground = false)
            }
        }
    }

    private suspend fun loadSettings(): MqttSettings? {
        return latest
            ?: runCatching { settingsStore.settings.first() }.getOrNull()?.also { latest = it }
    }

    private suspend fun ensureConnected(fromForeground: Boolean) {
        val s = loadSettings() ?: return
        if (!s.autoConnect) return
        if (s.host.isBlank() || s.user.isBlank() || s.password.isBlank()) return

        if (fromForeground) {
            reconnectFromForeground(s)
            return
        }

        if (mqtt.isConnected()) return
        if (mqtt.sessionActive()) return
        mqtt.connect(s)
    }

    private suspend fun reconnectFromForeground(s: MqttSettings) {
        val bgAt = backgroundedAtMs.getAndSet(0L)
        val bgMs = if (bgAt > 0L) System.currentTimeMillis() - bgAt else 0L

        val needFresh = when {
            // Delší pauza → socket skoro jistě mrtvý / doze.
            bgMs >= BACKGROUND_FORCE_RECONNECT_MS -> true
            !mqtt.isConnected() -> true
            mqtt.isStuckConnecting(STUCK_CONNECTING_MS) -> true
            else -> false
        }

        if (needFresh) {
            mqtt.connect(s)
            // Po doze první pokus občas selže — jeden retry.
            delay(CONNECT_VERIFY_MS)
            if (!mqtt.isConnected() && s.autoConnect) {
                mqtt.connect(s)
            }
            return
        }

        mqtt.publishWatch(true)
        delay(WATCH_VERIFY_MS)
        if (!mqtt.isConnected()) {
            mqtt.connect(s)
        }
    }

    companion object {
        /** Po tolika ms na pozadí vždy nový MQTT connect. */
        private const val BACKGROUND_FORCE_RECONNECT_MS = 15_000L
        private const val STUCK_CONNECTING_MS = 12_000L
        private const val CONNECT_VERIFY_MS = 4_000L
        private const val WATCH_VERIFY_MS = 2_000L
    }
}
