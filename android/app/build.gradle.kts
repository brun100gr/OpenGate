plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.opengate"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.opengate"
        minSdk = 23
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        // Backports java.time (API 26+) down to minSdk: without it the
        // timestamps in the MQTT payloads crash on Android 6.0–7.1
        isCoreLibraryDesugaringEnabled = true
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // Car App Library for Android Auto (apps "projected" onto the car screen)
    implementation("androidx.car.app:app:1.7.0")
    implementation("androidx.core:core-ktx:1.13.1")
    // Eclipse Paho Java MQTT client, used directly (without the deprecated
    // Android service): more than enough for a simple publish
    implementation("org.eclipse.paho:org.eclipse.paho.client.mqttv3:1.2.5")
    // Fused location provider: GPS/network/sensor fusion, used to stream the
    // position after the gate is opened
    implementation("com.google.android.gms:play-services-location:21.3.0")
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")
}
