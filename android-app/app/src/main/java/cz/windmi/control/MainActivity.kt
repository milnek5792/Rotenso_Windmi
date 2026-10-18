package cz.windmi.control

import android.graphics.Color
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.TextView
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.isVisible
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.google.android.material.textfield.TextInputEditText
import cz.windmi.control.databinding.ActivityMainBinding
import cz.windmi.control.screen.AppUiState
import cz.windmi.control.screen.MqttLinkStatus
import cz.windmi.control.screen.RegMode
import cz.windmi.control.screen.SigState
import cz.windmi.control.screen.WindmiViewModel
import cz.windmi.control.screen.formatOffset
import cz.windmi.control.screen.formatTemp
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var home: View
    private lateinit var settings: View

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
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val inflater = LayoutInflater.from(this)
        home = inflater.inflate(R.layout.view_home, binding.content, false)
        settings = inflater.inflate(R.layout.view_settings, binding.content, false)
        binding.content.addView(home)
        binding.content.addView(settings)
        settings.isVisible = false

        bindHome()
        bindSettings()
        bindNav()

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.ui.collect { render(it) }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        viewModel.onForeground(true)
    }

    override fun onPause() {
        viewModel.onForeground(false)
        super.onPause()
    }

    private fun bindNav() {
        binding.navHome.setOnClickListener { viewModel.selectTab(0) }
        binding.navSettings.setOnClickListener { viewModel.selectTab(1) }
    }

    private fun bindHome() {
        home.findViewById<Button>(R.id.btnMinus).setOnClickListener { viewModel.adjustSetpoint(-1) }
        home.findViewById<Button>(R.id.btnPlus).setOnClickListener { viewModel.adjustSetpoint(1) }
        home.findViewById<Button>(R.id.modeRoom).setOnClickListener { viewModel.setRegMode(RegMode.Room) }
        home.findViewById<Button>(R.id.modeEkv).setOnClickListener { viewModel.setRegMode(RegMode.Equitherm) }
        home.findViewById<Button>(R.id.modeWater).setOnClickListener { viewModel.setRegMode(RegMode.Water) }
        home.findViewById<Button>(R.id.btnStart).setOnClickListener { viewModel.setPower(true) }
        home.findViewById<Button>(R.id.btnStop).setOnClickListener { viewModel.setPower(false) }
    }

    private fun bindSettings() {
        settings.findViewById<Button>(R.id.btnConnect).setOnClickListener {
            syncDraftFromFields()
            viewModel.connect()
        }
        settings.findViewById<Button>(R.id.btnDisconnect).setOnClickListener {
            viewModel.disconnect()
        }
    }

    private fun syncDraftFromFields() {
        viewModel.updateDraft(
            host = settings.findViewById<TextInputEditText>(R.id.cfgHost).text?.toString().orEmpty(),
            user = settings.findViewById<TextInputEditText>(R.id.cfgUser).text?.toString().orEmpty(),
            password = settings.findViewById<TextInputEditText>(R.id.cfgPass).text?.toString().orEmpty(),
            prefix = settings.findViewById<TextInputEditText>(R.id.cfgPrefix).text?.toString().orEmpty(),
        )
    }

    private fun render(state: AppUiState) {
        home.isVisible = state.selectedTab == 0
        settings.isVisible = state.selectedTab == 1
        binding.navHome.setTextColor(if (state.selectedTab == 0) Color.parseColor("#0A84FF") else Color.parseColor("#8E8E93"))
        binding.navSettings.setTextColor(if (state.selectedTab == 1) Color.parseColor("#0A84FF") else Color.parseColor("#8E8E93"))

        val mqtt = state.mqttSig()
        val lin = state.linSig()
        binding.sigMqtt.text = state.flashMessage ?: "MQTT"
        binding.sigMqtt.setTextColor(sigColor(mqtt.first))
        binding.sigMb.setTextColor(sigColor(lin.first))

        binding.faultBanner.isVisible = state.faultVisible && state.faultText.isNotBlank()
        binding.faultBanner.text = "Poruchové hlášení\n${state.faultText}"

        val title = home.findViewById<TextView>(R.id.spTitle)
        val hint = home.findViewById<TextView>(R.id.spHint)
        when (state.regMode) {
            RegMode.Room -> {
                title.text = "Nastavení pokojové teploty"
                title.setTextColor(Color.parseColor("#0A84FF"))
                hint.isVisible = false
            }
            RegMode.Equitherm -> {
                title.text = "Ekvitermní SP vody"
                title.setTextColor(Color.parseColor("#64D2FF"))
                hint.isVisible = state.mqttConnected && state.eqOffset != null
                hint.text = "Korekce ${formatOffset(state.eqOffset)} °C"
            }
            RegMode.Water -> {
                title.text = "Nastavení teploty vody"
                title.setTextColor(Color.parseColor("#FF9F0A"))
                hint.isVisible = false
            }
        }
        home.findViewById<TextView>(R.id.spValue).text = state.displaySetpoint()

        val live = state.mqttConnected
        home.findViewById<TextView>(R.id.tempRoom).text =
            "Vnitřní\n${formatTemp(if (live) state.temps.room else null, 1)}"
        home.findViewById<TextView>(R.id.tempOutdoor).text =
            "Venkovní\n${formatTemp(if (live) state.temps.outdoor else null, 1)}"
        home.findViewById<TextView>(R.id.tempInlet).text =
            "Vstup\n${formatTemp(if (live) state.temps.inlet else null, 0)}"
        home.findViewById<TextView>(R.id.tempOutlet).text =
            "Výstup\n${formatTemp(if (live) state.temps.outlet else null, 0)}"

        paintStat(home.findViewById(R.id.statPower), state.power, false)
        paintStat(home.findViewById(R.id.statPump), state.pump, false)
        paintStat(home.findViewById(R.id.statCompressor), state.compressor, false)
        paintStat(home.findViewById(R.id.statDefrost), state.defrost, true)
        paintStat(home.findViewById(R.id.statElec), state.elec, true)

        val host = settings.findViewById<TextInputEditText>(R.id.cfgHost)
        val user = settings.findViewById<TextInputEditText>(R.id.cfgUser)
        val pass = settings.findViewById<TextInputEditText>(R.id.cfgPass)
        val prefix = settings.findViewById<TextInputEditText>(R.id.cfgPrefix)
        if (!host.isFocused) host.setText(state.brokerUrl)
        if (!user.isFocused) user.setText(state.brokerUser)
        if (!pass.isFocused) pass.setText(state.brokerPassword)
        if (!prefix.isFocused) prefix.setText(state.topicPrefix)

        settings.findViewById<Button>(R.id.btnConnect).isEnabled = !state.sessionActive
        settings.findViewById<Button>(R.id.btnDisconnect).isEnabled = state.sessionActive
        val status = settings.findViewById<TextView>(R.id.cfgStatus)
        status.text = state.settingsStatus
        status.setTextColor(
            when {
                state.mqttStatus == MqttLinkStatus.Error -> Color.parseColor("#FF453A")
                state.mqttConnected -> Color.parseColor("#30D158")
                else -> Color.parseColor("#8E8E93")
            },
        )
    }

    private fun paintStat(view: TextView, on: Boolean, warn: Boolean) {
        view.setTextColor(
            when {
                on && warn -> Color.parseColor("#FF9F0A")
                on -> Color.parseColor("#30D158")
                else -> Color.parseColor("#668E8E93")
            },
        )
    }

    private fun sigColor(state: SigState): Int = when (state) {
        SigState.Ok -> Color.parseColor("#30D158")
        SigState.Warn -> Color.parseColor("#FF9F0A")
        SigState.Error -> Color.parseColor("#FF453A")
        SigState.Off -> Color.parseColor("#8E8E93")
    }
}
