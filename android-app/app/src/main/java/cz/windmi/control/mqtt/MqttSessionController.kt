package cz.windmi.control.mqtt

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import cz.windmi.control.data.MqttSettings
import cz.windmi.control.data.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * MQTT session žije s procesem aplikace — ne s Activity/ViewModel.
 * Při zavření obrazovky jen watch OFF; při návratu znovu připojí, pokud je autoConnect.
 */
class MqttSessionController(
    private val settingsStore: SettingsStore,
    private val mqtt: MqttRepository,
    private val scope: CoroutineScope,
) {
    @Volatile
    private var latest: MqttSettings? = null

    private val lifecycle = object : DefaultLifecycleObserver {
        override fun onStart(owner: LifecycleOwner) {
            scope.launch { ensureConnected(fromForeground = true) }
        }

        override fun onStop(owner: LifecycleOwner) {
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

    private suspend fun ensureConnected(fromForeground: Boolean) {
        val s = latest ?: runCatching { settingsStore.settings.first() }.getOrNull()?.also { latest = it }
            ?: return
        if (!s.autoConnect) return
        if (s.host.isBlank() || s.user.isBlank() || s.password.isBlank()) return

        if (mqtt.isConnected()) {
            if (fromForeground) mqtt.publishWatch(true)
            return
        }
        // Už probíhá connect/reconnect — neruš novým killClient.
        if (mqtt.sessionActive()) return
        mqtt.connect(s)
    }
}
