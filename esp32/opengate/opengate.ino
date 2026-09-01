/*
 * OpenGate — gate side (ESP32)
 *
 * Connects to WiFi and to the MQTT broker (HiveMQ Cloud, TLS on port 8883),
 * subscribes to the command topic and, when the expected payload arrives,
 * closes a relay for PULSE_MS milliseconds: the equivalent of pressing the
 * open button on the gate control board.
 *
 * Credentials and broker details live in secrets.h (gitignored): copy
 * secrets.h.example to secrets.h and fill in your values.
 *
 * Required libraries (Arduino IDE Library Manager):
 *   - PubSubClient (Nick O'Leary)
 * WiFi and WiFiClientSecure are included in the ESP32 core.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#include "secrets.h"  // WiFi/broker credentials and root CA (not committed)

// ---------------------- Non-secret configuration ----------------------

const char* MQTT_TOPIC    = "opengate/cmd";
const char* MQTT_COMMAND  = "open";   // payload that triggers the relay

const int RELAY_PIN = 26;             // GPIO wired to the relay module IN pin
const unsigned long PULSE_MS = 1000;  // duration of the "button press" pulse

// -----------------------------------------------------------------------

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

void pulseRelay() {
  digitalWrite(RELAY_PIN, HIGH);
  delay(PULSE_MS);
  digitalWrite(RELAY_PIN, LOW);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("Message on %s (%u bytes)\n", topic, length);
  // Exact match: same length and same bytes as the expected command
  if (length == strlen(MQTT_COMMAND) &&
      memcmp(payload, MQTT_COMMAND, length) == 0) {
    Serial.println("Valid command: opening the gate");
    pulseRelay();
  } else {
    Serial.println("Unrecognized payload, ignored");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMqtt() {
  while (!mqtt.connected()) {
    // Unique client ID derived from the MAC, so two ESP32s never collide
    String clientId = "opengate-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("Connecting to MQTT at %s:%u... ", MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("OK");
      mqtt.subscribe(MQTT_TOPIC, 1);
      Serial.printf("Subscribed to %s\n", MQTT_TOPIC);
    } else {
      // rc: see PubSubClient MQTT_CONNECT_* constants (e.g. -2 = network, 5 = not authorized)
      Serial.printf("failed, rc=%d. Retrying in 3s\n", mqtt.state());
      delay(3000);
    }
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // relay at rest from boot

  Serial.begin(115200);
  delay(100);

  connectWiFi();

  if (strstr(ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    tlsClient.setCACert(ROOT_CA);
  } else {
    Serial.println("WARNING: no root CA configured, using setInsecure()");
    tlsClient.setInsecure();
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);
}

void loop() {
  connectWiFi();   // no-op if already connected
  if (!mqtt.connected()) {
    connectMqtt();
  }
  mqtt.loop();
}
