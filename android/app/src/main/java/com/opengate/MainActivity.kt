package com.opengate

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.TextView

/** Phone UI: a single button that sends the MQTT command. */
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var openButton: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        openButton = findViewById(R.id.openButton)
        openButton.setOnClickListener { openGate() }
    }

    private fun openGate() {
        openButton.isEnabled = false
        statusText.text = getString(R.string.sending)

        MqttPublisher.publish { success, error ->
            runOnUiThread {
                openButton.isEnabled = true
                statusText.text = if (success) {
                    getString(R.string.sent_ok)
                } else {
                    getString(R.string.sent_error, error)
                }
            }
        }
    }
}
