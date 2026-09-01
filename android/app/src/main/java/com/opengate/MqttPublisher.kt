package com.opengate

import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import java.util.concurrent.Executors

/**
 * Publishes the open command to the broker with a
 * connect-publish-disconnect strategy: for an occasional command this is
 * more robust than keeping a persistent connection from the phone.
 *
 * Network work runs on a dedicated thread (Android forbids networking on
 * the main thread); the [onResult] callback is invoked on that thread, so
 * the caller is responsible for hopping back to the main thread.
 */
object MqttPublisher {

    private val executor = Executors.newSingleThreadExecutor()

    fun publish(onResult: (success: Boolean, error: String?) -> Unit) {
        executor.execute {
            try {
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
                val message = MqttMessage(Config.COMMAND.toByteArray()).apply {
                    qos = 1            // at-least-once delivery
                    isRetained = false // never retained: an "open" command must not linger on the broker
                }
                client.publish(Config.TOPIC, message)
                client.disconnect()
                client.close()
                onResult(true, null)
            } catch (e: Exception) {
                onResult(false, e.message ?: e.javaClass.simpleName)
            }
        }
    }
}
