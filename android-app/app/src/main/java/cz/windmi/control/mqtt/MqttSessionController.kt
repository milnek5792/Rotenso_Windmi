package cz.windmi.control.mqtt

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner

/**
 * Na pozadí MQTT neběží — při odchodu z appky odpojit.
 * Připojení při otevření řídí ViewModel (stejná cesta jako tlačítko Připojit).
 */
class MqttSessionController(
    private val mqtt: MqttRepository,
) {
    private val lifecycle = object : DefaultLifecycleObserver {
        override fun onStop(owner: LifecycleOwner) {
            mqtt.disconnect(sendWatchOff = true)
        }
    }

    fun start() {
        ProcessLifecycleOwner.get().lifecycle.addObserver(lifecycle)
    }
}
