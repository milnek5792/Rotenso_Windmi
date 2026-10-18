package cz.windmi.control

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import cz.windmi.control.screen.WindmiApp
import cz.windmi.control.screen.WindmiViewModel
import cz.windmi.control.screen.theme.WindmiTheme

class MainActivity : ComponentActivity() {
    private val viewModel: WindmiViewModel by viewModels {
        val app = application as WindmiApplication
        object : ViewModelProvider.Factory {
            @Suppress("UNCHECKED_CAST")
            override fun <T : ViewModel> create(modelClass: Class<T>): T {
                return WindmiViewModel(app.settingsStore, app.mqttRepository) as T
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            WindmiTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    WindmiApp(viewModel = viewModel)
                }
            }
        }
    }
}
