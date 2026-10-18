package cz.windmi.control

import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.core.widget.ImageViewCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
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
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = ContextCompat.getColor(this, R.color.bg)
        window.navigationBarColor = ContextCompat.getColor(this, R.color.bg)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        applySystemInsets()

        bindHome()
        bindSettings()
        bindNav()

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.ui.collect { render(it) }
            }
        }
    }

    private fun applySystemInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            binding.topbar.updatePadding(top = bars.top)
            // Padding on the nav container, not shrinking the 56dp tab hit targets.
            binding.bottomNav.updatePadding(bottom = bars.bottom)
            insets
        }
    }

    private fun bindNav() {
        val goHome = View.OnClickListener { viewModel.selectTab(0) }
        val goSettings = View.OnClickListener { viewModel.selectTab(1) }
        binding.navHome.setOnClickListener(goHome)
        binding.navHomeIcon.setOnClickListener(goHome)
        binding.navHomeLabel.setOnClickListener(goHome)
        binding.navSettings.setOnClickListener(goSettings)
        binding.navSettingsIcon.setOnClickListener(goSettings)
        binding.navSettingsLabel.setOnClickListener(goSettings)
    }

    private fun bindHome() {
        binding.pageHome.root.findViewById<View>(R.id.btnMinus).setOnClickListener {
            viewModel.adjustSetpoint(-1)
        }
        binding.pageHome.root.findViewById<View>(R.id.btnPlus).setOnClickListener {
            viewModel.adjustSetpoint(1)
        }
        binding.pageHome.root.findViewById<View>(R.id.modeRoom).setOnClickListener {
            viewModel.setRegMode(RegMode.Room)
        }
        binding.pageHome.root.findViewById<View>(R.id.modeEkv).setOnClickListener {
            viewModel.setRegMode(RegMode.Equitherm)
        }
        binding.pageHome.root.findViewById<View>(R.id.modeWater).setOnClickListener {
            viewModel.setRegMode(RegMode.Water)
        }
        binding.pageHome.root.findViewById<View>(R.id.btnStart).setOnClickListener {
            viewModel.setPower(true)
        }
        binding.pageHome.root.findViewById<View>(R.id.btnStop).setOnClickListener {
            viewModel.setPower(false)
        }
    }

    private fun bindSettings() {
        binding.pageSettings.root.findViewById<View>(R.id.btnConnect).setOnClickListener {
            syncDraftFromFields()
            viewModel.connect()
        }
        binding.pageSettings.root.findViewById<View>(R.id.btnDisconnect).setOnClickListener {
            viewModel.disconnect()
        }
        val pass = settings().findViewById<EditText>(R.id.cfgPass)
        val toggle = settings().findViewById<TextView>(R.id.btnTogglePass)
        toggle.setOnClickListener {
            val visible = pass.transformationMethod == null
            if (visible) {
                pass.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                pass.transformationMethod =
                    android.text.method.PasswordTransformationMethod.getInstance()
                toggle.alpha = 0.55f
            } else {
                pass.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                pass.transformationMethod = null
                toggle.alpha = 1f
            }
            pass.setSelection(pass.text?.length ?: 0)
        }
        toggle.alpha = 0.55f
    }

    private fun home(): View = binding.pageHome.root
    private fun settings(): View = binding.pageSettings.root

    private fun syncDraftFromFields() {
        viewModel.updateDraft(
            host = settings().findViewById<EditText>(R.id.cfgHost).text?.toString().orEmpty(),
            user = settings().findViewById<EditText>(R.id.cfgUser).text?.toString().orEmpty(),
            password = settings().findViewById<EditText>(R.id.cfgPass).text?.toString().orEmpty(),
            prefix = settings().findViewById<EditText>(R.id.cfgPrefix).text?.toString().orEmpty(),
        )
    }

    private fun render(state: AppUiState) {
        val tab = state.selectedTab.coerceIn(0, 1)
        if (binding.pager.displayedChild != tab) {
            binding.pager.displayedChild = tab
        }

        val showHome = tab == 0
        val active = ContextCompat.getColor(this, R.color.room)
        val inactive = ContextCompat.getColor(this, R.color.muted)
        binding.navHomeIcon.setTextColor(if (showHome) active else inactive)
        binding.navHomeLabel.setTextColor(if (showHome) active else inactive)
        binding.navSettingsIcon.setTextColor(if (!showHome) active else inactive)
        binding.navSettingsLabel.setTextColor(if (!showHome) active else inactive)

        paintSig(binding.sigMqtt, state.flashMessage ?: "MQTT", state.mqttSig().first)
        paintSig(binding.sigMb, "MB", state.linSig().first)

        val showFault = state.faultVisible && state.faultText.isNotBlank()
        binding.faultBanner.isVisible = showFault
        binding.faultText.text = state.faultText

        val title = home().findViewById<TextView>(R.id.spTitle)
        val hint = home().findViewById<TextView>(R.id.spHint)
        when (state.regMode) {
            RegMode.Room -> {
                title.text = "Nastavení pokojové teploty"
                title.setTextColor(ContextCompat.getColor(this, R.color.room))
                hint.isVisible = false
            }
            RegMode.Equitherm -> {
                title.text = "Ekvitermní SP vody"
                title.setTextColor(ContextCompat.getColor(this, R.color.ekv))
                hint.isVisible = state.mqttConnected && state.eqOffset != null
                hint.text = "Korekce ${formatOffset(state.eqOffset)} °C"
            }
            RegMode.Water -> {
                title.text = "Nastavení teploty vody"
                title.setTextColor(ContextCompat.getColor(this, R.color.water))
                hint.isVisible = false
            }
        }
        home().findViewById<TextView>(R.id.spValue).text = state.displaySetpoint()
        paintModes(state.regMode)

        val live = state.mqttConnected
        home().findViewById<TextView>(R.id.tempRoom).text =
            formatTemp(if (live) state.temps.room else null, 1)
        home().findViewById<TextView>(R.id.tempOutdoor).text =
            formatTemp(if (live) state.temps.outdoor else null, 1)
        home().findViewById<TextView>(R.id.tempInlet).text =
            formatTemp(if (live) state.temps.inlet else null, 0)
        home().findViewById<TextView>(R.id.tempOutlet).text =
            formatTemp(if (live) state.temps.outlet else null, 0)

        home().findViewById<View>(R.id.btnStart).setBackgroundResource(
            if (state.power) R.drawable.bg_btn_start_on else R.drawable.bg_btn_start,
        )
        home().findViewById<View>(R.id.btnStop).setBackgroundResource(
            if (!state.power && state.mqttConnected) R.drawable.bg_btn_stop_on else R.drawable.bg_btn_stop,
        )

        paintStat(home().findViewById(R.id.statPower), state.power, warn = false)
        paintStat(home().findViewById(R.id.statPump), state.pump, warn = false)
        paintStat(home().findViewById(R.id.statCompressor), state.compressor, warn = false)
        paintStat(home().findViewById(R.id.statDefrost), state.defrost, warn = true)
        paintStat(home().findViewById(R.id.statElec), state.elec, warn = true)

        val host = settings().findViewById<EditText>(R.id.cfgHost)
        val user = settings().findViewById<EditText>(R.id.cfgUser)
        val pass = settings().findViewById<EditText>(R.id.cfgPass)
        val prefix = settings().findViewById<EditText>(R.id.cfgPrefix)
        if (!host.isFocused && host.text?.toString() != state.brokerUrl) host.setText(state.brokerUrl)
        if (!user.isFocused && user.text?.toString() != state.brokerUser) user.setText(state.brokerUser)
        if (!pass.isFocused && pass.text?.toString() != state.brokerPassword) pass.setText(state.brokerPassword)
        if (!prefix.isFocused && prefix.text?.toString() != state.topicPrefix) prefix.setText(state.topicPrefix)

        settings().findViewById<View>(R.id.btnConnect).isEnabled = !state.sessionActive
        settings().findViewById<View>(R.id.btnConnect).alpha = if (state.sessionActive) 0.45f else 1f
        settings().findViewById<View>(R.id.btnDisconnect).isEnabled = state.sessionActive
        settings().findViewById<View>(R.id.btnDisconnect).alpha = if (state.sessionActive) 1f else 0.45f

        val status = settings().findViewById<TextView>(R.id.cfgStatus)
        status.text = state.settingsStatus
        status.setTextColor(
            ContextCompat.getColor(
                this,
                when {
                    state.mqttStatus == MqttLinkStatus.Error -> R.color.fault
                    state.mqttConnected -> R.color.ok
                    else -> R.color.muted
                },
            ),
        )
    }

    private fun paintModes(mode: RegMode) {
        val room = home().findViewById<TextView>(R.id.modeRoom)
        val ekv = home().findViewById<TextView>(R.id.modeEkv)
        val water = home().findViewById<TextView>(R.id.modeWater)
        room.setBackgroundResource(if (mode == RegMode.Room) R.drawable.bg_chip_room else R.drawable.bg_chip)
        ekv.setBackgroundResource(if (mode == RegMode.Equitherm) R.drawable.bg_chip_ekv else R.drawable.bg_chip)
        water.setBackgroundResource(if (mode == RegMode.Water) R.drawable.bg_chip_water else R.drawable.bg_chip)
        room.setTextColor(
            ContextCompat.getColor(this, if (mode == RegMode.Room) android.R.color.white else R.color.muted),
        )
        ekv.setTextColor(
            ContextCompat.getColor(this, if (mode == RegMode.Equitherm) android.R.color.white else R.color.muted),
        )
        water.setTextColor(
            ContextCompat.getColor(this, if (mode == RegMode.Water) android.R.color.white else R.color.muted),
        )
    }

    private fun paintSig(view: TextView, label: String, state: SigState) {
        view.text = label
        val bg = GradientDrawable().apply {
            cornerRadius = dp(10f)
            setColor(
                ContextCompat.getColor(
                    this@MainActivity,
                    when (state) {
                        SigState.Ok -> R.color.sig_ok_bg
                        SigState.Warn -> R.color.sig_warn_bg
                        SigState.Error -> R.color.sig_error_bg
                        SigState.Off -> R.color.panel
                    },
                ),
            )
            setStroke(
                dp(1f).toInt(),
                ContextCompat.getColor(
                    this@MainActivity,
                    when (state) {
                        SigState.Ok -> R.color.sig_ok_border
                        SigState.Warn -> R.color.sig_warn_border
                        SigState.Error -> R.color.sig_error_border
                        SigState.Off -> R.color.border
                    },
                ),
            )
        }
        view.background = bg
        view.setTextColor(
            ContextCompat.getColor(
                this,
                when (state) {
                    SigState.Ok -> R.color.ok
                    SigState.Warn -> R.color.warn
                    SigState.Error -> R.color.fault
                    SigState.Off -> R.color.muted
                },
            ),
        )
    }

    private fun paintStat(view: View, on: Boolean, warn: Boolean) {
        view.setBackgroundResource(
            when {
                on && warn -> R.drawable.bg_stat_warn_on
                on -> R.drawable.bg_stat_on
                else -> R.drawable.bg_stat
            },
        )
        view.alpha = if (on) 1f else 0.45f
        if (view is ImageView) {
            val color = ContextCompat.getColor(
                this,
                when {
                    on && warn -> R.color.warn
                    on -> R.color.ok
                    else -> R.color.muted
                },
            )
            ImageViewCompat.setImageTintList(view, ColorStateList.valueOf(color))
        }
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density
}
