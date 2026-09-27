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
 * Subscribes to [Config.STATE_TOPIC] and publishes the gate state to whoever is
 * listening.
 *
 * The connection follows the listeners: it is opened when the first one
 * registers and closed when the last one leaves, so the broker is only kept on
 * the line while somebody cares — the phone UI on screen, or a running
 * [GpsTrackingService].
 *
 * The state is deliberately not persisted across commands: [reset] wipes it
 * when a new OPEN command goes out, otherwise a stale [GateState.OPEN] from the
 * previous session would cut the new position stream short.
 */
object GateStateMonitor {

    fun interface OnGateStateListener {
        fun onGateStateChanged(state: GateState)
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

    private val _state = MutableStateFlow(GateState.UNKNOWN)
    val state: StateFlow<GateState> = _state.asStateFlow()

    private var client: MqttClient? = null
    private var retry: ScheduledFuture<*>? = null

    private val callback = object : MqttCallbackExtended {
        override fun connectComplete(reconnect: Boolean, serverURI: String?) {
            // Only the automatic reconnects are handled here — the first
            // subscription is issued by ensureConnected(). The work is handed
            // to the executor because this runs on the Paho callback thread,
            // the very thread that would have to deliver the SUBACK: blocking
            // it with a synchronous subscribe() deadlocks the client for good.
            if (reconnect) {
                Log.d(TAG, "Reconnected to $serverURI, resubscribing")
                executor.execute { subscribe() }
            }
        }

        override fun messageArrived(topic: String?, message: MqttMessage?) {
            val payload = message?.payload?.toString(Charsets.UTF_8) ?: return
            Log.d(TAG, "Received on $topic: $payload")
            parseState(payload)?.let { publishState(it) }
        }

        override fun connectionLost(cause: Throwable?) {
            // isAutomaticReconnect takes it from here; the periodic check below
            // is the safety net for a connection that never came up at all.
            Log.w(TAG, "Connection lost: ${cause?.message}")
        }

        override fun deliveryComplete(token: IMqttDeliveryToken?) = Unit
    }

    /** Must run on [executor]: it blocks until the broker answers. */
    private fun subscribe() {
        val mqttClient = client ?: return
        try {
            val token = mqttClient.subscribeWithResponse(Config.STATE_TOPIC, 1)
            // A SUBACK can succeed at the protocol level and still refuse the
            // subscription: 0x80 means the broker does not let these
            // credentials read the topic, and no message will ever arrive.
            if (token.grantedQos.any { it == SUBSCRIPTION_REFUSED }) {
                Log.e(
                    TAG,
                    "Broker refused the subscription to ${Config.STATE_TOPIC} " +
                        "for user ${Config.USERNAME}: check the topic permissions"
                )
            } else {
                Log.d(
                    TAG,
                    "Subscribed to ${Config.STATE_TOPIC}, granted QoS ${token.grantedQos.joinToString()}"
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "Could not subscribe to ${Config.STATE_TOPIC}", e)
        }
    }

    fun addListener(listener: OnGateStateListener) {
        listeners.add(listener)
        listener.onGateStateChanged(_state.value)
        executor.execute { startWatching() }
    }

    fun removeListener(listener: OnGateStateListener) {
        listeners.remove(listener)
        executor.execute { if (listeners.isEmpty()) stopWatching() }
    }

    /** Forgets the last known state: called when a new OPEN command is sent. */
    fun reset() {
        publishState(GateState.UNKNOWN)
    }

    private fun publishState(newState: GateState) {
        if (_state.value == newState) return
        _state.value = newState
        Log.d(TAG, "Gate state: $newState")
        for (listener in listeners) {
            // Callbacks arrive on the MQTT thread, listeners update the UI
            mainHandler.post { listener.onGateStateChanged(newState) }
        }
    }

    private fun parseState(payload: String): GateState? {
        val raw = try {
            JSONObject(payload).optString("state")
        } catch (e: Exception) {
            Log.w(TAG, "Malformed payload: $payload")
            return null
        }

        return when (raw.lowercase()) {
            "waiting" -> GateState.WAITING
            "open" -> GateState.OPEN
            "unknown" -> GateState.UNKNOWN
            // A state we do not know about tells us nothing: keep the last one
            else -> {
                Log.w(TAG, "Unknown state '$raw', ignored")
                null
            }
        }
    }

    private fun startWatching() {
        ensureConnected()
        if (retry == null) {
            retry = executor.scheduleWithFixedDelay(
                { if (listeners.isNotEmpty()) ensureConnected() },
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
        // A client left over from a failed attempt would leak its resources
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
                // The phone is moving: mobile coverage is expected to drop out
                isAutomaticReconnect = true
            }

            // Assigned first: connectComplete() fires from inside connect()
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
