package com.opengate

import android.Manifest
import android.app.Activity
import android.graphics.Color
import android.location.Location
import android.os.Build
import android.os.Bundle
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.ContextCompat
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import java.util.Locale

/** Phone UI: a single button that sends the MQTT command. */
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var countdownText: TextView
    private lateinit var gateStateText: TextView
    private lateinit var distanceText: TextView
    private lateinit var openButton: Button

    private lateinit var fusedLocationClient: FusedLocationProviderClient

    private val uiLocationCallback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            result.lastLocation?.let { updateDistance(it) }
        }
    }

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

    private val gateStateListener = GateStateMonitor.OnGateStateListener { state ->
        val (label, color) = when (state) {
            GateState.UNKNOWN -> R.string.gate_state_unknown to R.color.gate_state_unknown
            GateState.WAITING -> R.string.gate_state_waiting to R.color.gate_state_waiting
            GateState.OPEN -> R.string.gate_state_open to R.color.gate_state_open
        }
        gateStateText.text = getString(label)
        gateStateText.setTextColor(ContextCompat.getColor(this, color))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        fusedLocationClient = LocationServices.getFusedLocationProviderClient(this)

        statusText = findViewById(R.id.statusText)
        countdownText = findViewById(R.id.countdownText)
        gateStateText = findViewById(R.id.gateStateText)
        distanceText = findViewById(R.id.distanceText)
        openButton = findViewById(R.id.openButton)
        openButton.setOnClickListener { openGate() }
    }

    override fun onStart() {
        super.onStart()
        GpsTrackingService.addOnCountdownListener(countdownListener)
        GateStateMonitor.addListener(gateStateListener)
        startLocationUpdates()
    }

    override fun onStop() {
        super.onStop()
        GpsTrackingService.removeOnCountdownListener(countdownListener)
        GateStateMonitor.removeListener(gateStateListener)
        stopLocationUpdates()
    }

    private fun startLocationUpdates() {
        if (!GpsTrackingService.hasLocationPermission(this)) return
        try {
            fusedLocationClient.lastLocation.addOnSuccessListener { loc ->
                if (loc != null) updateDistance(loc)
            }

            val request = LocationRequest.Builder(
                Priority.PRIORITY_HIGH_ACCURACY,
                2000L
            ).setMinUpdateIntervalMillis(1000L).build()

            fusedLocationClient.requestLocationUpdates(
                request,
                uiLocationCallback,
                Looper.getMainLooper()
            )
        } catch (_: SecurityException) {
            // Permission check covered above
        }
    }

    private fun stopLocationUpdates() {
        fusedLocationClient.removeLocationUpdates(uiLocationCallback)
    }

    private fun updateDistance(location: Location) {
        val homeLocation = Location("").apply {
            latitude = Config.LATITUDE
            longitude = Config.LONGITUDE
        }
        val distanceMeters = location.distanceTo(homeLocation).toDouble()

        val textStr = if (distanceMeters >= 1000) {
            getString(R.string.distance_format_km, distanceMeters / 1000.0)
        } else {
            getString(R.string.distance_format, distanceMeters)
        }

        distanceText.text = textStr
        distanceText.setTextColor(getDistanceColor(distanceMeters))
    }

    private fun getDistanceColor(distanceMeters: Double): Int {
        val hue = when {
            distanceMeters >= 200.0 -> 0.0f  // Rosso (> 200m)
            distanceMeters <= 100.0 -> 120.0f // Verde (<= 100m)
            else -> {
                // Sfumatura tra 200m e 100m: 0° (Rosso) -> Arancione -> Giallo -> 120° (Verde)
                val t = ((200.0 - distanceMeters) / 100.0).toFloat()
                t * 120.0f
            }
        }
        return Color.HSVToColor(floatArrayOf(hue, 1.0f, 0.85f))
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
        } else {
            startLocationUpdates()
            if (!GpsTrackingService.start(this)) {
                toast(R.string.gps_unavailable)
            }
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
