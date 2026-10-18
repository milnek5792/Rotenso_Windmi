package cz.windmi.control.mqtt

import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.datatypes.MqttQos
import com.hivemq.client.mqtt.lifecycle.MqttDisconnectSource
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import com.hivemq.client.mqtt.mqtt3.message.publish.Mqtt3Publish
import cz.windmi.control.data.MqttSettings
import cz.windmi.control.data.SettingsStore
import cz.windmi.control.screen.MqttLinkStatus
import cz.windmi.control.screen.TelePatch
import java.net.URI
import java.nio.charset.StandardCharsets
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.roundToInt

sealed interface MqttEvent {
    data class Status(val status: MqttLinkStatus, val error: String = "") : MqttEvent
    data class Connection(val connected: Boolean, val reconnecting: Boolean) : MqttEvent
    data class Tele(val patch: TelePatch) : MqttEvent
}

enum class PublishResult { Ok, Queued, Fail }

class MqttRepository {
    private val listeners = AtomicReference<((MqttEvent) -> Unit)?>(null)
    private val clientRef = AtomicReference<Mqtt3AsyncClient?>(null)
    private val prefixRef = AtomicReference("windmi")
    private val userStopped = AtomicBoolean(true)
    private val sessionGen = AtomicInteger(0)
    private val hydrateUntilMs = AtomicReference(0L)
    private val pending = ConcurrentLinkedQueue<Pair<String, String>>()
    private val scheduler = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "windmi-mqtt").apply { isDaemon = true }
    }
    private var watchFuture: ScheduledFuture<*>? = null
    private var hydrateFuture: ScheduledFuture<*>? = null
    private val lastStatus = AtomicReference(MqttLinkStatus.Idle)

    fun setListener(listener: ((MqttEvent) -> Unit)?) {
        listeners.set(listener)
    }

    fun isConnected(): Boolean = clientRef.get()?.state?.isConnected == true

    fun sessionActive(): Boolean {
        if (userStopped.get()) return false
        val status = lastStatus.get()
        return status == MqttLinkStatus.Connected || status == MqttLinkStatus.Connecting
    }

    fun connect(settings: MqttSettings) {
        // Zruš předchozí klienta (vč. auto-reconnect), ať nekazí novou session.
        killClient(sendWatchOff = false)
        val gen = sessionGen.incrementAndGet()
        userStopped.set(false)

        val prefix = SettingsStore.normalizePrefix(settings.prefix)
        prefixRef.set(prefix)
        emit(MqttEvent.Status(MqttLinkStatus.Connecting))

        val endpoint = parseEndpoint(MqttSettings.normalizeBrokerUrl(settings.host))
        if (endpoint == null) {
            emit(MqttEvent.Status(MqttLinkStatus.Error, "Neplatná adresa brokeru"))
            return
        }

        val clientId = "WindmiAndroid_" + UUID.randomUUID().toString().take(8)
        var builder = MqttClient.builder()
            .useMqttVersion3()
            .identifier(clientId)
            .serverHost(endpoint.host)
            .serverPort(endpoint.port)
            .automaticReconnectWithDefaultConfig()
            .addConnectedListener {
                if (!isCurrent(gen)) return@addConnectedListener
                beginHydrate()
                emit(MqttEvent.Status(MqttLinkStatus.Connected))
                subscribeAll(gen) {
                    if (!isCurrent(gen) || !isConnected()) return@subscribeAll
                    publishWatch(true)
                    startWatchKeepalive()
                    flushPending()
                    // Retained availability někdy dorazí až po SUBACK — druhý watch po chvíli.
                    scheduler.schedule({
                        if (isCurrent(gen) && isConnected()) publishWatch(true)
                    }, 1500, TimeUnit.MILLISECONDS)
                }
            }
            .addDisconnectedListener { ctx ->
                if (!isCurrent(gen)) return@addDisconnectedListener
                stopWatchKeepalive()
                if (userStopped.get()) {
                    if (lastStatus.get() != MqttLinkStatus.Error) {
                        emit(MqttEvent.Status(MqttLinkStatus.Idle))
                    }
                    emit(MqttEvent.Connection(connected = false, reconnecting = false))
                    return@addDisconnectedListener
                }
                if (ctx.source != MqttDisconnectSource.USER) {
                    emit(MqttEvent.Status(MqttLinkStatus.Connecting, "Obnovuji spojení…"))
                    emit(MqttEvent.Connection(connected = false, reconnecting = true))
                }
            }

        if (endpoint.ssl) {
            builder = builder.sslWithDefaultConfig()
        }
        if (endpoint.websocket) {
            builder = builder.webSocketConfig()
                .serverPath(endpoint.path)
                .applyWebSocketConfig()
        }

        val client = builder.buildAsync()
        clientRef.set(client)

        var connectBuilder = client.connectWith()
            .cleanSession(true)
            .keepAlive(45)
        if (settings.user.isNotBlank()) {
            connectBuilder = connectBuilder.simpleAuth()
                .username(settings.user)
                .password(settings.password.toByteArray(StandardCharsets.UTF_8))
                .applySimpleAuth()
        }

        connectBuilder.send()
            .whenComplete { _, err ->
                if (err != null && isCurrent(gen) && !userStopped.get()) {
                    emit(MqttEvent.Status(MqttLinkStatus.Error, friendlyError(err)))
                }
            }
    }

    fun disconnect(sendWatchOff: Boolean = true, notifyIdle: Boolean = true) {
        killClient(sendWatchOff = sendWatchOff)
        if (notifyIdle) {
            emit(MqttEvent.Status(MqttLinkStatus.Idle))
            emit(MqttEvent.Connection(connected = false, reconnecting = false))
        }
    }

    private fun killClient(sendWatchOff: Boolean) {
        userStopped.set(true)
        sessionGen.incrementAndGet() // invaliduje listenery starého klienta
        pending.clear()
        stopWatchKeepalive()
        endHydrate()
        val client = clientRef.getAndSet(null) ?: return
        if (sendWatchOff && client.state.isConnected) {
            runCatching {
                client.publishWith()
                    .topic(topic("cmd/watch"))
                    .payload("OFF".toByteArray(StandardCharsets.UTF_8))
                    .qos(MqttQos.AT_MOST_ONCE)
                    .send()
            }
        }
        // USER disconnect vypne automaticReconnect u tohoto klienta.
        runCatching { client.disconnect() }
    }

    private fun isCurrent(gen: Int): Boolean = sessionGen.get() == gen

    fun publishWatch(on: Boolean) {
        val client = clientRef.get() ?: return
        if (!client.state.isConnected) return
        client.publishWith()
            .topic(topic("cmd/watch"))
            .payload((if (on) "ON" else "OFF").toByteArray(StandardCharsets.UTF_8))
            .qos(MqttQos.AT_LEAST_ONCE)
            .send()
    }

    fun publishCmd(suffix: String, payload: String): PublishResult {
        if (userStopped.get()) return PublishResult.Fail
        val client = clientRef.get()
        if (client?.state?.isConnected == true) {
            publishCmdNow(suffix, payload)
            return PublishResult.Ok
        }
        if (sessionActive()) {
            enqueue(suffix, payload)
            return PublishResult.Queued
        }
        return PublishResult.Fail
    }

    private fun publishCmdNow(suffix: String, payload: String) {
        val client = clientRef.get() ?: return
        if (suffix != "watch") {
            publishWatch(true)
        }
        client.publishWith()
            .topic(topic("cmd/$suffix"))
            .payload(payload.toByteArray(StandardCharsets.UTF_8))
            .qos(MqttQos.AT_LEAST_ONCE)
            .send()
            .whenComplete { _, err ->
                if (err != null) {
                    enqueue(suffix, payload)
                }
            }
    }

    private fun enqueue(suffix: String, payload: String) {
        pending.add(suffix to payload)
        while (pending.size > MAX_PENDING) {
            pending.poll()
        }
    }

    private fun flushPending() {
        val client = clientRef.get() ?: return
        if (!client.state.isConnected) return
        while (true) {
            val cmd = pending.poll() ?: break
            publishCmdNow(cmd.first, cmd.second)
        }
    }

    private fun subscribeAll(gen: Int, onReady: () -> Unit) {
        val client = clientRef.get() ?: return
        val left = AtomicInteger(SUBSCRIBE_FILTERS.size)
        for (suffix in SUBSCRIBE_FILTERS) {
            client.subscribeWith()
                .topicFilter(topic(suffix))
                .qos(MqttQos.AT_LEAST_ONCE)
                .callback(::onPublish)
                .send()
                .whenComplete { _, _ ->
                    if (left.decrementAndGet() == 0 && isCurrent(gen)) {
                        onReady()
                    }
                }
        }
    }

    private fun onPublish(publish: Mqtt3Publish) {
        val topicName = publish.topic.toString()
        val payload = String(publish.payloadAsBytes, StandardCharsets.UTF_8)
        handleMessage(topicName, payload, publish.isRetain)
    }

    private fun handleMessage(fullTopic: String, raw: String, retained: Boolean) {
        val base = "${prefixRef.get()}/"
        if (!fullTopic.startsWith(base)) return
        val rel = fullTopic.removePrefix(base)
        if (retained && shouldSkipRetained(rel)) return
        val msg = raw.trim()
        val patch = when (rel) {
            "availability" -> TelePatch(tab5Online = msg.equals("online", ignoreCase = true))
            "tele/temp_room" -> parseTemp(msg)?.let { TelePatch(room = it) }
            "tele/temp_outdoor" -> parseTemp(msg)?.let { TelePatch(outdoor = it) }
            "tele/temp_inlet" -> parseTemp(msg)?.let { TelePatch(inlet = it.roundToInt().toFloat()) }
            "tele/temp_outlet" -> parseTemp(msg)?.let { TelePatch(outlet = it.roundToInt().toFloat()) }
            "tele/temp_set" -> {
                val v = parseTemp(msg)
                when {
                    v != null -> TelePatch(setpoint = v, clearSetpoint = false)
                    msg == TELE_NA || msg == "---" || msg.equals("off", true) ->
                        TelePatch(clearSetpoint = true)
                    else -> null
                }
            }
            "tele/reg_mode" -> TelePatch(regMode = normalizeRegMode(msg))
            "tele/eq_offset" -> parseTemp(msg)?.let { TelePatch(eqOffset = it) }
            "tele/power" -> TelePatch(power = parseOnOff(msg))
            "tele/pump" -> TelePatch(pump = parseOnOff(msg))
            "tele/compressor" -> TelePatch(compressor = parseOnOff(msg))
            "tele/lin" -> TelePatch(lin = parseOnOff(msg))
            "tele/defrost" -> TelePatch(defrost = parseOnOff(msg))
            "tele/elec_heat" -> TelePatch(elec = parseOnOff(msg))
            "tele/alarm" -> TelePatch(alarm = parseOnOff(msg))
            "tele/porucha" -> TelePatch(poruchaText = msg)
            "tele/watch" -> TelePatch(watchActive = parseOnOff(msg))
            else -> null
        } ?: return
        emit(MqttEvent.Tele(patch))
    }

    private fun shouldSkipRetained(rel: String): Boolean {
        return System.currentTimeMillis() < hydrateUntilMs.get() &&
            (rel == "tele/alarm" || rel == "tele/porucha")
    }

    private fun beginHydrate() {
        endHydrate()
        hydrateUntilMs.set(System.currentTimeMillis() + 3500)
        hydrateFuture = scheduler.schedule({
            hydrateUntilMs.set(0)
            if (isConnected()) publishWatch(true)
        }, 3600, TimeUnit.MILLISECONDS)
    }

    private fun endHydrate() {
        hydrateFuture?.cancel(false)
        hydrateFuture = null
        hydrateUntilMs.set(0)
    }

    private fun startWatchKeepalive() {
        stopWatchKeepalive()
        watchFuture = scheduler.scheduleAtFixedRate({
            if (isConnected()) publishWatch(true)
        }, 45, 45, TimeUnit.SECONDS)
    }

    private fun stopWatchKeepalive() {
        watchFuture?.cancel(false)
        watchFuture = null
    }

    private fun topic(suffix: String): String = "${prefixRef.get()}/$suffix"

    private fun emit(event: MqttEvent) {
        if (event is MqttEvent.Status) {
            lastStatus.set(event.status)
        }
        listeners.get()?.invoke(event)
    }

    private data class Endpoint(
        val host: String,
        val port: Int,
        val path: String,
        val ssl: Boolean,
        val websocket: Boolean,
    )

    private fun parseEndpoint(rawUrl: String): Endpoint? {
        return try {
            val uri = URI(rawUrl.trim())
            val scheme = (uri.scheme ?: "").lowercase()
            val host = uri.host ?: return null
            val websocket = scheme == "ws" || scheme == "wss"
            val ssl = scheme == "wss" || scheme == "mqtts" || scheme == "ssl" || scheme == "mqtt+ssl"
            val defaultPort = when (scheme) {
                "wss" -> 443
                "ws" -> 80
                "mqtts", "ssl", "mqtt+ssl" -> 8883
                else -> 1883
            }
            val port = if (uri.port > 0) uri.port else defaultPort
            val path = (uri.rawPath ?: "mqtt")
                .trim()
                .trimStart('/')
                .ifBlank { "mqtt" }
            Endpoint(host, port, path, ssl, websocket)
        } catch (_: Exception) {
            null
        }
    }

    private fun friendlyError(err: Throwable): String {
        var cur: Throwable? = err
        while (cur?.cause != null && cur.cause !== cur) {
            cur = cur.cause
        }
        val msg = (cur?.message ?: err.message).orEmpty()
        return when {
            msg.contains("WebSocket", ignoreCase = true) ->
                "WebSocket selhal — použij mqtts://…:8883"
            msg.contains("SSL", ignoreCase = true) ||
                msg.contains("TLS", ignoreCase = true) ||
                msg.contains("certificate", ignoreCase = true) ->
                "TLS chyba při spojení s brokerem"
            msg.contains("UnknownHost", ignoreCase = true) ||
                msg.contains("unable to resolve", ignoreCase = true) ->
                "DNS: host brokeru nenalezen"
            msg.contains("timeout", ignoreCase = true) ||
                msg.contains("Timed out", ignoreCase = true) ->
                "Timeout připojení k brokeru"
            msg.contains("Not authorized", ignoreCase = true) ||
                msg.contains("Bad user", ignoreCase = true) ||
                msg.contains("CONNACK", ignoreCase = true) ->
                "Odmítnuto brokerem (uživatel/heslo?)"
            msg.isBlank() -> "Chyba MQTT"
            else -> msg.take(120)
        }
    }

    companion object {
        private val SUBSCRIBE_FILTERS = listOf("availability", "tele/#")
        private const val MAX_PENDING = 8
    }
}
