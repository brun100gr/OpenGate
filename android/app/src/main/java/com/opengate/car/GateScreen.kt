package com.opengate.car

import android.os.Handler
import android.os.Looper
import androidx.car.app.CarContext
import androidx.car.app.CarToast
import androidx.car.app.Screen
import androidx.car.app.model.Action
import androidx.car.app.model.CarIcon
import androidx.car.app.model.GridItem
import androidx.car.app.model.GridTemplate
import androidx.car.app.model.ItemList
import androidx.car.app.model.Template
import androidx.core.graphics.drawable.IconCompat
import com.opengate.GpsTrackingService
import com.opengate.MqttPublisher
import com.opengate.R

/**
 * Screen shown on the car display: a single grid cell with the gate icon.
 * With Android Auto you don't draw a free-form UI; you describe a template
 * ([GridTemplate]) that the host renders on its own. To refresh the screen
 * you change the state and call [invalidate].
 */
class GateScreen(carContext: CarContext) : Screen(carContext) {

    private val mainHandler = Handler(Looper.getMainLooper())
    private var sending = false

    override fun onGetTemplate(): Template {
        val itemBuilder = GridItem.Builder()
            .setTitle(carContext.getString(R.string.open_gate))

        if (sending) {
            itemBuilder.setLoading(true)
        } else {
            itemBuilder
                .setImage(
                    CarIcon.Builder(
                        IconCompat.createWithResource(carContext, R.drawable.ic_gate)
                    ).build(),
                    GridItem.IMAGE_TYPE_ICON
                )
                .setOnClickListener { openGate() }
        }

        return GridTemplate.Builder()
            .setTitle(carContext.getString(R.string.app_name))
            .setHeaderAction(Action.APP_ICON)
            .setSingleList(ItemList.Builder().addItem(itemBuilder.build()).build())
            .build()
    }

    private fun openGate() {
        sending = true
        invalidate()

        MqttPublisher.publish { success, _ ->
            // The callback arrives on the MQTT thread: invalidate() and
            // CarToast must be called on the main thread.
            mainHandler.post {
                sending = false
                invalidate()
                CarToast.makeText(
                    carContext,
                    carContext.getString(
                        if (success) R.string.sent_ok else R.string.sent_error_short
                    ),
                    CarToast.LENGTH_SHORT
                ).show()
            }
        }

        // Permission dialogs have no place on a car screen: the location
        // permission is granted once from the phone UI. Without it the gate
        // still opens, only the position stream is skipped.
        if (!GpsTrackingService.start(carContext)) {
            CarToast.makeText(
                carContext,
                carContext.getString(R.string.gps_unavailable_short),
                CarToast.LENGTH_SHORT
            ).show()
        }
    }
}
