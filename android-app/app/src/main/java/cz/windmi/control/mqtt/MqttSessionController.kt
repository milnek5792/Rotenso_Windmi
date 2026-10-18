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

/**
 * MQTT jen když je appka v popředí.
 * onStop = odpojit; onStart = znovu připojit (autoConnect + uložené údaje).
 */
class MqttSessionController(
    private val settingsStore: SettingsStore,
    private val mqtt: MqttRepository,
    private val scope: CoroutineScope,
) {
    @Volatile
    private var latest: MqttSettings? = null

    @Volatile
    private var appInForeground = false

    private val lifecycle = object : DefaultLifecycleObserver {
        override fun onStart(owner: LifecycleOwner) {
            appInForeground = true
            scope.launch { connectIfNeeded(retry = true) }
        }

        override fun onStop(owner: LifecycleOwner) {
            appInForeground = false
            // Na pozadí neběží — čistý disconnect (včetně watch OFF).
            mqtt.disconnect(sendWatchOff = true)
        }
    }

    fun start() {
        ProcessLifecycleOwner.get().lifecycle.addObserver(lifecycle)
        scope.launch {
            settingsStore.settings.collectLatest { s ->
                latest = s
                if (appInForeground) {
                    connectIfNeeded(retry = false)
                }
            }
        }
    }

    private suspend fun loadSettings(): MqttSettings? {
        return latest
            ?: runCatching { settingsStore.settings.first() }.getOrNull()?.also { latest = it }
    }

    private suspend fun connectIfNeeded(retry: Boolean) {
        val s = loadSettings() ?: return
        if (!s.autoConnect) return
        if (s.host.isBlank() || s.user.isBlank() || s.password.isBlank()) return
        if (!appInForeground) return

        mqtt.connect(s)
        if (!retry) return

        delay(CONNECT_RETRY_MS)
        if (appInForeground && s.autoConnect && !mqtt.isConnected()) {
            mqtt.connect(s)
        }
    }

    companion object {
        private const val CONNECT_RETRY_MS = 3_500L
    }
}
