package com.opengate

import android.util.Log
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONObject
import java.time.Instant
import java.util.UUID
import java.util.concurrent.Executors

/**
 * Publishes the OPEN command to the broker with a connect-publish-disconnect strategy.
 * For an occasional command this is more robust than keeping a persistent connection from the phone.
 *
 * The command is sent as a JSON payload with:
 * - id: unique UUID for this command
 * - command: "OPEN" (or other command type)
 * - timestamp: ISO 8601 UTC timestamp
 *
 * Network work runs on a dedicated thread (Android forbids networking on the main thread);
 * the [onResult] callback is invoked on that thread, so the caller is responsible for
 * hopping back to the main thread.
 */
object MqttPublisher {

    private const val TAG = "MqttPublisher"
    private val executor = Executors.newSingleThreadExecutor()

    fun publish(onResult: (success: Boolean, error: String?) -> Unit) {
        executor.execute {
            try {
                val commandId = UUID.randomUUID().toString()
                val payload = createPayload(commandId)
                val payloadJson = payload.toString()

                Log.d(TAG, "Publishing to ${Config.CMD_TOPIC}")
                Log.d(TAG, "Payload: $payloadJson")

                val client = MqttClient(
                    Config.BROKER_URI,
                    MqttClient.generateClientId(),
                    MemoryPersistence()
                )

                val options = MqttConnectOptions().apply {
                    userName = Config.USERNAME
                    password = Config.PASSWORD.toCharArray()
                    isCleanSession = true
                    connectionTimeout = 10
                }

                client.connect(options)
                Log.d(TAG, "Connected to broker")

                val message = MqttMessage(payloadJson.toByteArray()).apply {
                    qos = 1            // at-least-once delivery
                    isRetained = false // never retained: an "OPEN" command must not linger on the broker
                }

                client.publish(Config.CMD_TOPIC, message)
                Log.d(TAG, "Command published with ID: $commandId")

                client.disconnect()
                client.close()

                onResult(true, null)
            } catch (e: Exception) {
                Log.e(TAG, "Error publishing command", e)
                onResult(false, e.message ?: e.javaClass.simpleName)
            }
        }
    }

    private fun createPayload(commandId: String): JSONObject {
        return JSONObject().apply {
            put("id", commandId)
            put("command", Config.COMMAND)
            put("timestamp", Instant.now().toString())
        }
    }
}

