package cz.windmi.control

import android.app.Application
import cz.windmi.control.data.SettingsStore
import cz.windmi.control.mqtt.MqttRepository

class WindmiApplication : Application() {
    lateinit var settingsStore: SettingsStore
        private set
    lateinit var mqttRepository: MqttRepository
        private set

    override fun onCreate() {
        super.onCreate()
        settingsStore = SettingsStore(this)
        mqttRepository = MqttRepository()
    }
}
