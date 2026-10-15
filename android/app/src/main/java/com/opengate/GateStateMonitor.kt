package com.opengate

import android.os.Handler
import android.os.Looper
import android.util.Log
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONObject
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Gate state reported by the ESP32 on [Config.STATE_TOPIC]. */
enum class GateState {
    /** Nothing received yet: the ESP32 has not spoken since the last command. */
    UNKNOWN,

    /** The ESP32 is awake and waiting for the car to come close. */
    WAITING,

    /** The gate has been opened: the position is no longer needed. */
    OPEN
}

/**
 * Subscribes to [Config.STATE_TOPIC] and [Config.DISTANCE_RECEIVED_TOPIC]
 * and publishes state and distance updates to whoever is listening.
 *
 * The connection follows the listeners: it is opened when the first one
 * registers and closed when the last one leaves, so the broker is only kept on
 * the line while somebody cares — the phone UI on screen, or a running
 * [GpsTrackingService].
 */
object GateStateMonitor {

    fun interface OnGateStateListener {
        fun onGateStateChanged(state: GateState)
    }

    fun interface OnDistanceReceivedListener {
        fun onDistanceReceived(distanceMeters: Double)
    }

    private const val TAG = "GateStateMonitor"

    /** Retry cadence used until the first connection succeeds. */
    private const val RETRY_INTERVAL_S = 10L

    /** Granted-QoS value a broker returns when it rejects a subscription. */
    private const val SUBSCRIPTION_REFUSED = 128

    /** Owns every MQTT call, so the fields below need no locking. */
    private val executor = Executors.newSingleThreadScheduledExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<OnGateStateListener>()
    private val distanceListeners = CopyOnWriteArrayList<OnDistanceReceivedListener>()

    private val _state = MutableStateFlow(GateState.UNKNOWN)
    val state: StateFlow<GateState> = _state.asStateFlow()

    private val _receivedDistance = MutableStateFlow<Double?>(null)
    val receivedDistance: StateFlow<Double?> = _receivedDistance.asStateFlow()

    private var client: MqttClient? = null
    private var retry: ScheduledFuture<*>? = null

    private val callback = object : MqttCallbackExtended {
        override fun connectComplete(reconnect: Boolean, serverURI: String?) {
            Log.d(TAG, "Connect complete (reconnect=$reconnect) to $serverURI")
            executor.execute { subscribe() }
        }

        override fun messageArrived(topic: String?, message: MqttMessage?) {
            val payload = message?.payload?.toString(Charsets.UTF_8) ?: return
            Log.d(TAG, "Message arrived on topic [$topic]: '$payload'")

            if (topic.equals(Config.STATE_TOPIC, ignoreCase = true)) {
                val newState = parseState(payload)
                Log.d(TAG, "Parsed gate state: $newState from payload '$payload'")
                if (newState != null) {
                    publishState(newState)
                }
            } else if (topic.equals(Config.DISTANCE_RECEIVED_TOPIC, ignoreCase = true)) {
                val distance = parseDistance(payload)
                Log.d(TAG, "Parsed distance: $distance from payload '$payload'")
                if (distance != null) {
                    publishReceivedDistance(distance)
                }
            }
        }

        override fun connectionLost(cause: Throwable?) {
            Log.w(TAG, "Connection lost: ${cause?.message}")
        }

        override fun deliveryComplete(token: IMqttDeliveryToken?) = Unit
    }

    /** Must run on [executor]: it blocks until the broker answers. */
    private fun subscribe() {
        val mqttClient = client ?: return
        if (!mqttClient.isConnected) return

        subscribeToTopic(mqttClient, Config.STATE_TOPIC)
        subscribeToTopic(mqttClient, Config.DISTANCE_RECEIVED_TOPIC)
    }

    private fun subscribeToTopic(mqttClient: MqttClient, topicName: String) {
        try {
            val token = mqttClient.subscribeWithResponse(topicName, 1)
            if (token.grantedQos.any { it == SUBSCRIPTION_REFUSED }) {
                Log.e(TAG, "Broker refused subscription to $topicName for user ${Config.USERNAME}")
            } else {
                Log.d(TAG, "Subscribed successfully to $topicName (granted QoS ${token.grantedQos.joinToString()})")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error subscribing to $topicName", e)
        }
    }

    fun addListener(listener: OnGateStateListener) {
        listeners.add(listener)
        listener.onGateStateChanged(_state.value)
        executor.execute { startWatching() }
    }

    fun removeListener(listener: OnGateStateListener) {
        listeners.remove(listener)
        executor.execute { if (listeners.isEmpty() && distanceListeners.isEmpty()) stopWatching() }
    }

    fun addDistanceListener(listener: OnDistanceReceivedListener) {
        distanceListeners.add(listener)
        _receivedDistance.value?.let { distance ->
            mainHandler.post { listener.onDistanceReceived(distance) }
        }
        executor.execute { startWatching() }
    }

    fun removeDistanceListener(listener: OnDistanceReceivedListener) {
        distanceListeners.remove(listener)
        executor.execute { if (listeners.isEmpty() && distanceListeners.isEmpty()) stopWatching() }
    }

    /** Forgets the last known state: called when a new OPEN command is sent. */
    fun reset() {
        publishState(GateState.UNKNOWN)
    }

    private fun publishState(newState: GateState) {
        if (_state.value == newState) return
        _state.value = newState
        Log.d(TAG, "Gate state published: $newState")
        for (listener in listeners) {
            mainHandler.post { listener.onGateStateChanged(newState) }
        }
    }

    private fun publishReceivedDistance(distance: Double) {
        _receivedDistance.value = distance
        Log.d(TAG, "Received distance published: $distance m")
        for (listener in distanceListeners) {
            mainHandler.post { listener.onDistanceReceived(distance) }
        }
    }

    private fun parseState(payload: String): GateState? {
        val trimmed = payload.trim()
        val raw = if (trimmed.startsWith("{")) {
            try {
                val json = JSONObject(trimmed)
                if (json.has("state")) {
                    json.optString("state")
                } else if (json.has("status")) {
                    json.optString("status")
                } else if (json.has("gate")) {
                    json.optString("gate")
                } else {
                    trimmed
                }
            } catch (e: Exception) {
                trimmed
            }
        } else {
            trimmed
        }

        return when (raw.lowercase().trim()) {
            "waiting", "\"waiting\"" -> GateState.WAITING
            "open", "\"open\"" -> GateState.OPEN
            "unknown", "\"unknown\"" -> GateState.UNKNOWN
            else -> {
                Log.w(TAG, "Unknown state '$raw' in payload '$payload'")
                null
            }
        }
    }

    private fun parseDistance(payload: String): Double? {
        val trimmed = payload.trim()
        if (trimmed.startsWith("{")) {
            try {
                val json = JSONObject(trimmed)
                if (json.has("distance")) return json.getDouble("distance")
                if (json.has("dist")) return json.getDouble("dist")
                if (json.has("value")) return json.getDouble("value")
                if (json.has("meters")) return json.getDouble("meters")
            } catch (e: Exception) {
                Log.w(TAG, "Malformed JSON distance payload: $payload")
            }
        }
        val cleanStr = trimmed.replace("\"", "").replace("m", "").trim()
        return cleanStr.toDoubleOrNull()
    }

    private fun startWatching() {
        ensureConnected()
        if (retry == null) {
            retry = executor.scheduleWithFixedDelay(
                { if (listeners.isNotEmpty() || distanceListeners.isNotEmpty()) ensureConnected() },
                RETRY_INTERVAL_S,
                RETRY_INTERVAL_S,
                TimeUnit.SECONDS
            )
        }
    }

    private fun stopWatching() {
        retry?.cancel(false)
        retry = null
        closeQuietly()
        Log.d(TAG, "No listeners left, disconnected")
    }

    private fun ensureConnected() {
        if (client?.isConnected == true) return
        closeQuietly()

        try {
            val mqttClient = MqttClient(
                Config.BROKER_URI,
                MqttClient.generateClientId(),
                MemoryPersistence()
            )
            mqttClient.setCallback(callback)

            val options = MqttConnectOptions().apply {
                userName = Config.USERNAME
                password = Config.PASSWORD.toCharArray()
                isCleanSession = true
                connectionTimeout = 10
                isAutomaticReconnect = true
            }

            client = mqttClient
            mqttClient.connect(options)
            Log.d(TAG, "Connected to broker as ${Config.USERNAME}")
            subscribe()
        } catch (e: Exception) {
            Log.w(TAG, "Could not connect: ${e.message}")
            closeQuietly()
        }
    }

    private fun closeQuietly() {
        val mqttClient = client ?: return
        client = null
        try {
            if (mqttClient.isConnected) {
                mqttClient.disconnect()
            }
            mqttClient.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing the MQTT client", e)
        }
    }
}
