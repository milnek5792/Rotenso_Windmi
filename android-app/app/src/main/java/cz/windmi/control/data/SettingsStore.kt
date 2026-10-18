package cz.windmi.control.data

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.dataStore by preferencesDataStore(name = "windmi_settings")

class SettingsStore(private val context: Context) {
    private val keyHost = stringPreferencesKey("host")
    private val keyUser = stringPreferencesKey("user")
    private val keyPassword = stringPreferencesKey("password")
    private val keyPrefix = stringPreferencesKey("prefix")
    private val keyAuto = booleanPreferencesKey("auto_connect")

    val settings: Flow<MqttSettings> = context.dataStore.data.map { prefs ->
        MqttSettings(
            host = MqttSettings.normalizeBrokerUrl(
                prefs[keyHost] ?: MqttSettings.DEFAULT_BROKER,
            ),
            user = prefs[keyUser] ?: "",
            password = prefs[keyPassword] ?: "",
            prefix = prefs[keyPrefix] ?: "windmi",
            autoConnect = prefs[keyAuto] ?: true,
        )
    }

    suspend fun save(settings: MqttSettings) {
        context.dataStore.edit { prefs ->
            prefs[keyHost] = MqttSettings.normalizeBrokerUrl(settings.host)
            prefs[keyUser] = settings.user
            prefs[keyPassword] = settings.password
            prefs[keyPrefix] = normalizePrefix(settings.prefix)
            prefs[keyAuto] = settings.autoConnect
        }
    }

    suspend fun setAutoConnect(enabled: Boolean) {
        context.dataStore.edit { prefs ->
            prefs[keyAuto] = enabled
        }
    }

    companion object {
        fun normalizePrefix(prefix: String): String {
            val cleaned = prefix.trim().trimEnd('/').lowercase()
            return cleaned.ifEmpty { "windmi" }
        }
    }
}
