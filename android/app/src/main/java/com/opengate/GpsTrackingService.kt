package com.opengate

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * Streams the phone position to the broker for [SESSION_DURATION_MS] after the
 * gate is opened, one message per second.
 *
 * It is a foreground service because the interesting case is the car: screen
 * off, app in the background. Without the foreground state (and its
 * notification) Android throttles location updates down to a handful per hour.
 *
 * The 1 Hz cadence comes from a timer publishing the most recent fix, not from
 * the location callbacks: the fused provider slows down on its own when the
 * phone sits still, and the requirement here is a message every second.
 */
class GpsTrackingService : Service() {

    private lateinit var fusedClient: FusedLocationProviderClient
    private lateinit var worker: ScheduledExecutorService
    private lateinit var publisher: GpsPublisher

    @Volatile
    private var lastLocation: Location? = null

    /** Elapsed-real-time instant at which the session ends. */
    @Volatile
    private var deadline = 0L

    private var tracking = false

    private val locationCallback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            result.lastLocation?.let { lastLocation = it }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        fusedClient = LocationServices.getFusedLocationProviderClient(this)
        worker = Executors.newSingleThreadScheduledExecutor()
        publisher = GpsPublisher(UUID.randomUUID().toString())
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Must happen within a few seconds of startForegroundService(), so it
        // comes first: it only fails if the location permission was revoked
        // between the button press and now.
        try {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                buildNotification(),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
                } else {
                    0
                }
            )
        } catch (e: Exception) {
            Log.e(TAG, "Could not go foreground", e)
            stopSelf()
            return START_NOT_STICKY
        }

        // A second press only pushes the deadline forward: the MQTT
        // connection, the session id and the sequence counter carry on.
        deadline = SystemClock.elapsedRealtime() + SESSION_DURATION_MS

        if (tracking) {
            Log.d(TAG, "Session extended by another press")
        } else {
            tracking = true
            startTracking()
        }
        return START_NOT_STICKY
    }

    private fun startTracking() {
        worker.execute { publisher.connect() }

        val request = LocationRequest.Builder(
            Priority.PRIORITY_HIGH_ACCURACY,
            PUBLISH_INTERVAL_MS
        ).setMinUpdateIntervalMillis(PUBLISH_INTERVAL_MS).build()

        try {
            fusedClient.requestLocationUpdates(request, locationCallback, Looper.getMainLooper())
        } catch (e: SecurityException) {
            Log.e(TAG, "Location permission revoked", e)
            stopSelf()
            return
        }

        // Fixed delay rather than fixed rate: if a publish stalls we do not
        // want the missed ticks to fire back to back in a burst afterwards.
        worker.scheduleWithFixedDelay(
            { tick() },
            PUBLISH_INTERVAL_MS,
            PUBLISH_INTERVAL_MS,
            TimeUnit.MILLISECONDS
        )
        Log.d(TAG, "Tracking started for ${SESSION_DURATION_MS / 1000} s")
    }

    private fun tick() {
        if (SystemClock.elapsedRealtime() >= deadline) {
            Log.d(TAG, "Session over")
            stopSelf()
            return
        }

        val location = lastLocation
        if (location == null) {
            // No fix yet: a gap is better than an invented position
            Log.d(TAG, "No fix yet, nothing to publish")
            return
        }

        publisher.publish(location)
    }

    override fun onDestroy() {
        fusedClient.removeLocationUpdates(locationCallback)
        // Queued before the shutdown so it still runs, and on the worker
        // thread because disconnecting talks to the network.
        worker.execute {
            publisher.disconnect()
            Log.d(TAG, "Session ended: ${publisher.published} published, ${publisher.dropped} dropped")
        }
        // Also cancels the periodic tick, which is not carried over a shutdown
        worker.shutdown()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManagerCompat.from(this).createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.gps_channel_name),
                    // Silent: this fires on every single gate opening
                    NotificationManager.IMPORTANCE_LOW
                )
            )
        }

        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.gps_notification_title))
            .setContentText(getString(R.string.gps_notification_text))
            .setSmallIcon(R.drawable.ic_gate)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val TAG = "GpsTrackingService"
        private const val CHANNEL_ID = "opengate_gps"
        private const val NOTIFICATION_ID = 1
        private const val PUBLISH_INTERVAL_MS = 1_000L
        private const val SESSION_DURATION_MS = 5 * 60 * 1_000L

        fun hasLocationPermission(context: Context): Boolean =
            ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED

        /**
         * Starts a tracking session, or extends the running one. Returns false
         * when the position cannot be streamed, so the caller can say so: the
         * gate has been opened anyway, GPS is the optional half of the press.
         */
        fun start(context: Context): Boolean {
            if (!hasLocationPermission(context)) {
                Log.w(TAG, "Location permission not granted, GPS stream skipped")
                return false
            }

            return try {
                ContextCompat.startForegroundService(
                    context,
                    Intent(context, GpsTrackingService::class.java)
                )
                true
            } catch (e: Exception) {
                // Android 12+ forbids starting a foreground service from the
                // background. It should not happen here (the phone UI is
                // visible, the car session is active) but it must never crash.
                Log.e(TAG, "Could not start the tracking service", e)
                false
            }
        }
    }
}
