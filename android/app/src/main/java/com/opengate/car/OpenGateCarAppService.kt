package com.opengate.car

import android.content.Intent
import androidx.car.app.CarAppService
import androidx.car.app.Screen
import androidx.car.app.Session
import androidx.car.app.validation.HostValidator

/**
 * Entry point for Android Auto: when the car launches the app, this service
 * creates a [Session] which in turn provides the initial screen.
 */
class OpenGateCarAppService : CarAppService() {

    override fun createHostValidator(): HostValidator {
        // ALLOW_ALL accepts any host (required for apps installed outside
        // the Play Store in developer mode). A Play Store release should use
        // Google's official allowlist instead.
        return HostValidator.ALLOW_ALL_HOSTS_VALIDATOR
    }

    override fun onCreateSession(): Session = object : Session() {
        override fun onCreateScreen(intent: Intent): Screen = GateScreen(carContext)
    }
}
