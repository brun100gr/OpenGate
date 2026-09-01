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
}
