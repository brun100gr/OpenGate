/*
 * OpenGate — gate side (ESP32)
 *
 * Intermittently wakes from deep sleep (2 min intervals), connects to WiFi and MQTT,
 * waits for a command, executes it with command deduplication (NVS), publishes ACK,
 * and returns to deep sleep.
 *
 * MQTT uses a persistent session (clean-session=false) with a fixed client ID,
 * allowing commands published while offline to be delivered upon wake.
 *
 * Credentials and broker details live in secrets.h (gitignored): copy
 * secrets.h.example to secrets.h and fill in your values.
 *
 * Required libraries (Arduino IDE Library Manager):
 *   - PubSubClient (Nick O'Leary)
 * WiFi, WiFiClientSecure, and Preferences are included in the ESP32 core.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

#include "secrets.h"

// ======================== Configuration ========================

const char* MQTT_CMD_TOPIC = "opengate/cmd";
const char* MQTT_ACK_TOPIC = "opengate/ack";

const int RELAY_PIN = 26;
const unsigned long PULSE_MS = 1000;

// Deep sleep duration: 2 minutes in microseconds
const uint64_t DEEP_SLEEP_DURATION_US = 2ULL * 60 * 1000000;

// MQTT awake timeout: wait this long for a message during wake
const unsigned long MQTT_AWAKE_TIMEOUT_MS = 10000;

// Maximum number of processed command IDs to store
const int MAX_PROCESSED_IDS = 20;

// =================================================================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
Preferences nvs;

String clientId;
volatile bool messageProcessed = false;

struct Message {
  char id[64];
  char command[32];
  bool valid;
};

// ============================= NVS ==============================

void nvsInit() {
  nvs.begin("opengate", false);
}

bool hasProcessedId(const String& id) {
  String ids = nvs.getString("cmd_ids", "");
  return ids.indexOf(id) != -1;
}

void saveProcessedId(const String& id) {
  String ids = nvs.getString("cmd_ids", "");

  if (ids.length() > 0) {
    ids = id + "," + ids;
  } else {
    ids = id;
  }

  // Keep only the last MAX_PROCESSED_IDS by counting backwards
  int commaCount = 0;
  for (int i = ids.length() - 1; i >= 0; i--) {
    if (ids[i] == ',') {
      commaCount++;
      if (commaCount >= MAX_PROCESSED_IDS) {
        ids = ids.substring(0, i);
        break;
      }
    }
  }

  nvs.putString("cmd_ids", ids);
  Serial.printf("[NVS] Processed IDs saved: %s\r\n", ids.c_str());
}

void saveWiFiInfo() {
  if (WiFi.status() == WL_CONNECTED) {
    nvs.putString("wifi_ssid", WiFi.SSID());
    nvs.putInt("wifi_channel", WiFi.channel());
  }
}

// ===================== Message Parsing ========================

bool extractJsonString(const char* json, const char* key, char* value, int maxLen) {
  // Extracts value from JSON like: {"key": "value"}
  String keyPattern = "\"" + String(key) + "\":\"";
  String jsonStr(json);

  int start = jsonStr.indexOf(keyPattern);
  if (start == -1) return false;

  start += keyPattern.length();
  int end = jsonStr.indexOf("\"", start);
  if (end == -1) return false;

  int len = end - start;
  if (len >= maxLen) len = maxLen - 1;

  jsonStr.substring(start, start + len).toCharArray(value, len + 1);
  return true;
}

Message parseMessage(const byte* payload, unsigned int length) {
  Message msg = {"", "", false};

  if (length > 512) {
    Serial.println("[MSG] Payload too large");
    return msg;
  }

  char* buffer = (char*)malloc(length + 1);
  if (!buffer) return msg;

  memcpy(buffer, payload, length);
  buffer[length] = '\0';

  if (extractJsonString(buffer, "id", msg.id, sizeof(msg.id)) &&
      extractJsonString(buffer, "command", msg.command, sizeof(msg.command))) {
    msg.valid = true;
  }

  free(buffer);
  return msg;
}

// ===================== GPIO & Relay ===========================

void pulseRelay() {
  Serial.println("[GPIO] Activating relay");
  digitalWrite(RELAY_PIN, HIGH);
  delay(PULSE_MS);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("[GPIO] Relay deactivated");
}

// ===================== MQTT ===================================

const char* getMqttStateString(int state) {
  switch (state) {
    case -4: return "MQTT_CONNECT_FAILED";
    case -3: return "MQTT_CONNECTION_REFUSED";
    case -2: return "MQTT_CONNECTION_LOST";
    case -1: return "MQTT_DISCONNECTED";
    case 0:  return "MQTT_CONNECTED";
    case 1:  return "MQTT_CONNECT_BAD_PROTOCOL";
    case 2:  return "MQTT_CONNECT_BAD_CLIENT_ID";
    case 3:  return "MQTT_CONNECT_UNAVAILABLE";
    case 4:  return "MQTT_CONNECT_BAD_CREDENTIALS";
    case 5:  return "MQTT_CONNECT_UNAUTHORIZED";
    default: return "MQTT_UNKNOWN";
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s (%u bytes)\r\n", topic, length);

  // Log raw payload
  char rawPayload[512];
  if (length >= sizeof(rawPayload)) {
    Serial.println("[MQTT] Payload too large to log");
  } else {
    memcpy(rawPayload, payload, length);
    rawPayload[length] = '\0';
    Serial.printf("[MQTT] Raw payload: %s\r\n", rawPayload);
  }

  Message msg = parseMessage(payload, length);
  if (!msg.valid) {
    Serial.println("[MSG] Missing id or command field");
    return;
  }

  String id(msg.id);
  String command(msg.command);

  Serial.printf("[MSG] id=%s, command=%s\r\n", msg.id, msg.command);

  // Deduplication check
  if (hasProcessedId(id)) {
    Serial.printf("[CMD] DUPLICATE id=%s\r\n", msg.id);
    publishAck(id, "DUPLICATE");
  } else if (command == "OPEN") {
    Serial.println("[CMD] Executing OPEN");
    pulseRelay();
    saveProcessedId(id);
    publishAck(id, "OK");
  } else {
    Serial.printf("[CMD] Unknown command: %s\r\n", msg.command);
    publishAck(id, "UNKNOWN_COMMAND");
  }

  messageProcessed = true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Already connected");
    return;
  }

  String savedSsid = nvs.getString("wifi_ssid", "");
  int savedChannel = nvs.getInt("wifi_channel", -1);

  WiFi.mode(WIFI_STA);

  if (savedSsid.length() > 0 && savedChannel > 0) {
    Serial.printf("[WiFi] Connecting to %s (cached channel %d)\r\n", savedSsid.c_str(), savedChannel);
    WiFi.begin(savedSsid.c_str(), WIFI_PASSWORD, savedChannel);
  } else {
    Serial.printf("[WiFi] Connecting to %s\r\n", WIFI_SSID);
    if (WIFI_CHANNEL >= 0) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  unsigned long timeout = millis() + 10000;  // 10 second timeout
  while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\r\n[WiFi] Connected, IP: %s\r\n", WiFi.localIP().toString().c_str());
    saveWiFiInfo();
  } else {
    Serial.println("\r\n[WiFi] Connection timeout");
  }
}

void connectMqtt() {
  int attempts = 0;
  const int MAX_ATTEMPTS = 3;

  while (!mqtt.connected() && attempts < MAX_ATTEMPTS) {
    Serial.printf("[MQTT] Connecting (attempt %d/%d)...\r\n", attempts + 1, MAX_ATTEMPTS);

    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("[MQTT] Connected to broker");

      // Subscribe with QoS 1
      if (mqtt.subscribe(MQTT_CMD_TOPIC, 1)) {
        Serial.printf("[MQTT] Successfully subscribed to %s (QoS 1)\r\n", MQTT_CMD_TOPIC);
      } else {
        Serial.printf("[MQTT] ERROR: Failed to subscribe to %s\r\n", MQTT_CMD_TOPIC);
      }

      return;
    }

    int rc = mqtt.state();
    Serial.printf("[MQTT] Connection failed (state=%d, rc=%s)\r\n", rc, getMqttStateString(rc));
    attempts++;
    if (attempts < MAX_ATTEMPTS) {
      delay(1000);
    }
  }

  if (!mqtt.connected()) {
    Serial.println("[MQTT] ERROR: Failed to connect after all retry attempts");
  }
}

void publishAck(const String& id, const String& result) {
  if (!mqtt.connected()) {
    Serial.println("[ACK] MQTT not connected, cannot publish");
    return;
  }

  // Build ISO 8601 timestamp
  time_t now = time(nullptr);
  struct tm* timeinfo = gmtime(&now);
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", timeinfo);

  // Build JSON manually
  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"result\":\"%s\",\"timestamp\":\"%s\"}",
           id.c_str(), result.c_str(), timestamp);

  if (mqtt.publish(MQTT_ACK_TOPIC, payload, 1)) {  // QoS 1
    Serial.printf("[ACK] Published: %s\r\n", payload);
  } else {
    Serial.println("[ACK] Failed to publish");
  }
}

// ===================== Setup & Loop ============================

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  Serial.begin(115200);
  delay(100);

  Serial.println("\r\n================== ESP32 WAKE UP ==================");

  // Initialize NVS
  nvsInit();

  // Generate stable client ID from MAC
  clientId = "opengate-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[Init] Client ID: %s\r\n", clientId.c_str());

  // Connect to WiFi
  connectWiFi();

  // Setup MQTT
  if (strstr(ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    tlsClient.setCACert(ROOT_CA);
  } else {
    Serial.println("[TLS] No root CA configured, using setInsecure()");
    tlsClient.setInsecure();
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);

  // Connect to MQTT
  connectMqtt();

  // Verify MQTT connection before waiting for messages
  if (!mqtt.connected()) {
    Serial.println("[MQTT] ERROR: Not connected to MQTT broker, skipping wait");
  } else {
    // Wait for message with timeout
    messageProcessed = false;
    unsigned long startTime = millis();
    unsigned long loopCount = 0;

    Serial.printf("[MQTT] Waiting for command (timeout=%lu ms)\r\n", MQTT_AWAKE_TIMEOUT_MS);

    while (millis() - startTime < MQTT_AWAKE_TIMEOUT_MS) {
      if (mqtt.connected()) {
        mqtt.loop();
        loopCount++;
        if (messageProcessed) {
          break;
        }
      } else {
        Serial.println("[MQTT] Connection lost during wait");
        break;
      }
      delay(50);
    }

    Serial.printf("[MQTT] Wait ended after %lu ms (%lu loops)\r\n", millis() - startTime, loopCount);

    if (!messageProcessed) {
      Serial.println("[MSG] No command received");
    }

    // Disconnect MQTT gracefully
    if (mqtt.connected()) {
      mqtt.disconnect();
      Serial.println("[MQTT] Disconnected");
    }
  }

  // Close NVS
  nvs.end();

  // Enter deep sleep
  Serial.printf("\r\n[Sleep] Going to deep sleep for 2 minutes...\r\n");
  Serial.flush();

  esp_deep_sleep(DEEP_SLEEP_DURATION_US);
  // Never reached
}

void loop() {
  // Never reached due to deep sleep
}
