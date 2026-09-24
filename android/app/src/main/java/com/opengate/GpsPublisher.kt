package com.opengate

import android.location.Location
import android.util.Log
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONObject
import java.time.Instant

/**
 * Publishes the stream of GPS fixes that follows an OPEN command.
 *
 * Unlike [MqttPublisher], which connects and disconnects around a single
 * command, this one holds the connection open for the whole tracking session:
 * reconnecting once per second would cost far more than the fixes themselves.
 *
 * Every message carries the [sessionId] and an incrementing sequence number,
 * so a subscriber can tell one session from the next and spot the messages
 * lost on the way — with QoS 0 losses are expected, not a malfunction.
 *
 * Not thread safe: every method must be called from the single background
 * thread owned by [GpsTrackingService] (Android forbids networking on the
 * main thread).
 */
class GpsPublisher(private val sessionId: String) {

    private var client: MqttClient? = null
    private var sequence = 0

    /** Fixes that reached the broker. */
    var published = 0
        private set

    /** Fixes dropped because the broker was unreachable. */
    var dropped = 0
        private set

    fun connect(): Boolean {
        return try {
            val mqttClient = MqttClient(
                Config.BROKER_URI,
                MqttClient.generateClientId(),
                MemoryPersistence()
            )

            val options = MqttConnectOptions().apply {
                userName = Config.USERNAME
                password = Config.PASSWORD.toCharArray()
                isCleanSession = true
                connectionTimeout = 10
                // The phone is moving: mobile coverage is expected to drop out
                isAutomaticReconnect = true
            }

            mqttClient.connect(options)
            client = mqttClient
            Log.d(TAG, "Connected to broker, session $sessionId")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Error connecting for session $sessionId", e)
            false
        }
    }

    /**
     * Publishes one fix. Failures are counted and swallowed: with QoS 0 a lost
     * position is acceptable, and a retry would only ever deliver a stale one —
     * the next fix is a second away.
     */
    fun publish(location: Location) {
        val payloadJson = createPayload(location, sequence++).toString()
        try {
            val mqttClient = client ?: throw IllegalStateException("MQTT client not connected")

            val message = MqttMessage(payloadJson.toByteArray()).apply {
                qos = 0            // fire and forget: a lost fix is replaced one second later
                isRetained = false // never retained: a stale position must not linger on the broker
            }

            mqttClient.publish(Config.GPS_TOPIC, message)
            published++
            Log.d(TAG, "Published: $payloadJson")
        } catch (e: Exception) {
            dropped++
            Log.w(TAG, "Dropped fix: ${e.message}")
        }
    }

    fun disconnect() {
        try {
            client?.disconnect()
            client?.close()
            Log.d(TAG, "Disconnected, session $sessionId")
        } catch (e: Exception) {
            Log.w(TAG, "Error closing the MQTT client", e)
        } finally {
            client = null
        }
    }

    private fun createPayload(location: Location, seq: Int): JSONObject {
        return JSONObject().apply {
            put("id", sessionId)
            put("seq", seq)
            // 6 decimals is ~11 cm, well beyond what a phone GPS can resolve
            put("lat", round(location.latitude, 6))
            put("lon", round(location.longitude, 6))
            if (location.hasAccuracy()) {
                put("accuracy", round(location.accuracy.toDouble(), 1)) // metres
            }
            if (location.hasSpeed()) {
                put("speed", round(location.speed.toDouble(), 1))       // metres per second
            }
            put("timestamp", Instant.now().toString())
        }
    }

    private fun round(value: Double, decimals: Int): Double {
        val factor = Math.pow(10.0, decimals.toDouble())
        return Math.round(value * factor) / factor
    }

    private companion object {
        const val TAG = "GpsPublisher"
    }
}
