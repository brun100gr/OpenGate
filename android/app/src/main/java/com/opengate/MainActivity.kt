package com.opengate

import android.Manifest
import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import java.util.Locale

/** Phone UI: a single button that sends the MQTT command. */
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var countdownText: TextView
    private lateinit var openButton: Button

    private val countdownListener = GpsTrackingService.OnCountdownListener { secondsLeft ->
        if (secondsLeft > 0) {
            val minutes = secondsLeft / 60
            val seconds = secondsLeft % 60
            val formattedTime = String.format(Locale.getDefault(), "%02d:%02d", minutes, seconds)
            countdownText.text = getString(R.string.gps_countdown, formattedTime)
            countdownText.visibility = View.VISIBLE
        } else {
            countdownText.visibility = View.GONE
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        countdownText = findViewById(R.id.countdownText)
        openButton = findViewById(R.id.openButton)
        openButton.setOnClickListener { openGate() }
    }

    override fun onStart() {
        super.onStart()
        GpsTrackingService.addOnCountdownListener(countdownListener)
    }

    override fun onStop() {
        super.onStop()
        GpsTrackingService.removeOnCountdownListener(countdownListener)
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

        startGpsTracking()
    }

    /**
     * Starts the GPS stream that follows the command. Deliberately independent
     * of the publish above: the gate must open even when the position cannot
     * be sent. Feedback goes through a toast so it does not race with
     * [statusText], which belongs to the command.
     */
    private fun startGpsTracking() {
        if (!GpsTrackingService.hasLocationPermission(this)) {
            requestPermissions(trackingPermissions(), REQUEST_TRACKING_PERMISSIONS)
            return
        }
        if (!GpsTrackingService.start(this)) {
            toast(R.string.gps_unavailable)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        if (requestCode != REQUEST_TRACKING_PERMISSIONS) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults)
            return
        }

        // The gate has already been opened at this point: a refusal only costs
        // the position stream.
        if (!GpsTrackingService.hasLocationPermission(this)) {
            toast(R.string.gps_permission_denied)
        } else if (!GpsTrackingService.start(this)) {
            toast(R.string.gps_unavailable)
        }
    }

    private fun trackingPermissions(): Array<String> {
        val permissions = mutableListOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            // Since API 31 the fine permission is only granted when the coarse
            // one is requested alongside it
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Without it the service still runs, but its notification stays
            // hidden and the tracking becomes invisible to the user
            permissions += Manifest.permission.POST_NOTIFICATIONS
        }
        return permissions.toTypedArray()
    }

    private fun toast(messageId: Int) {
        Toast.makeText(this, getString(messageId), Toast.LENGTH_LONG).show()
    }

    private companion object {
        const val REQUEST_TRACKING_PERMISSIONS = 1
    }
}
