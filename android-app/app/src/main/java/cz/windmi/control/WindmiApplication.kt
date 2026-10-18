package cz.windmi.control

import android.app.Application
import cz.windmi.control.data.SettingsStore
import cz.windmi.control.mqtt.MqttRepository
import cz.windmi.control.mqtt.MqttSessionController
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

class WindmiApplication : Application() {
    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    lateinit var settingsStore: SettingsStore
        private set
    lateinit var mqttRepository: MqttRepository
        private set

    private lateinit var mqttSession: MqttSessionController

    override fun onCreate() {
        super.onCreate()
        settingsStore = SettingsStore(this)
        mqttRepository = MqttRepository()
        mqttSession = MqttSessionController(settingsStore, mqttRepository, appScope)
        mqttSession.start()
    }
}
