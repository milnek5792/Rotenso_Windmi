package cz.windmi.control

import android.app.Application
import cz.windmi.control.data.SettingsStore
import cz.windmi.control.mqtt.MqttRepository
import cz.windmi.control.mqtt.MqttSessionController

class WindmiApplication : Application() {
    lateinit var settingsStore: SettingsStore
        private set
    lateinit var mqttRepository: MqttRepository
        private set

    private lateinit var mqttSession: MqttSessionController

    override fun onCreate() {
        super.onCreate()
        settingsStore = SettingsStore(this)
        mqttRepository = MqttRepository()
        mqttSession = MqttSessionController(mqttRepository)
        mqttSession.start()
    }
}
