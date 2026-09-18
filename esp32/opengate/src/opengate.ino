/*
 * OpenGate — gate side (ESP32)
 *
 * Intermittently wakes from deep sleep (2 min intervals), connects to WiFi and MQTT,
 * waits for a command, executes it with durable command reservation/deduplication (NVS),
 * publishes an MQTT ACK, sends a Telegram notification, and returns to deep sleep.
 *
 * MQTT uses a persistent session (clean-session=false) with a fixed client ID,
 * allowing queued QoS 1 commands published while the ESP32 is offline to be
 * delivered upon wake.
 *
 * IMPORTANT:
 * MQTT QoS 1 gives at-least-once delivery. The command transaction below therefore
 * implements an AT-MOST-ONCE hardware action across resets:
 *
 *   1. Persist command as PENDING in NVS.
 *   2. Activate the relay.
 *   3. Persist command as PROCESSED and clear PENDING.
 *   4. Publish ACK + Telegram notification.
 *
 * If the ESP32 resets after step 1, the command is deliberately NOT executed again
 * on the next boot. Without a physical gate-position/relay feedback signal, it is
 * impossible to know whether a reset happened before or after the relay pulse.
 * This design therefore prefers preventing a possible second gate opening over
 * guaranteeing that every command is eventually executed.
 *
 * IMPORTANT (wake flow):
 * Each wake is split into two distinct phases:
 *
 *   1. COLLECT — every command the broker has queued is read and PUBACKed, so
 *      none of them is delivered again at the next wake. Nothing is executed
 *      and no hardware moves during this phase.
 *   2. PROCESS — the collected commands are classified, the ones that will not
 *      run are acknowledged straight away, and only then does the hardware move.
 *
 * Within a single wake at most one command with an irreversible effect is
 * executed: the first one that arrived. Every further one is acknowledged as
 * DUPLICATE, exactly like a command whose ID had already been processed.
 *
 * The split matters because the hardware action blocks for several seconds.
 * Executing it while commands were still unacknowledged in the broker queue
 * left the MQTT session unserviced long enough for the broker to reset the TLS
 * connection, which then lost the ACKs that had to follow.
 *
 * IMPORTANT (PUBACK timing):
 * PubSubClient sends the QoS 1 PUBACK only AFTER the message callback returns.
 * The callback therefore does nothing but parse the command and store it in the
 * collection buffer; the relay pulse, the MQTT ACK and the Telegram
 * notification all run afterwards, from setup(). Doing that slow work inside
 * the callback would hold back the PUBACK, and if the MQTT socket died in the
 * meantime the broker would never receive it and would re-deliver the same
 * command on every reconnection.
 *
 * The Telegram notification is also deferred until after the MQTT connection is
 * closed: two concurrent mbedTLS sessions need ~40-50 KB of heap each, which is
 * enough to make the handshake fail on an ESP32.
 *
 * New command types are added to COMMAND_TABLE; see the "Command dispatch"
 * section.
 *
 * Credentials and certificates live in secrets.h (gitignored): copy
 * secrets.h.example to secrets.h and fill in your values.
 *
 * Required libraries (Arduino IDE Library Manager):
 *   - PubSubClient (Nick O'Leary)
 *   - UniversalTelegramBot
 *   - WiFi, WiFiClientSecure, and Preferences are included in the ESP32 core.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <ESP32Servo.h>
#include <time.h>

#include "secrets.h"

// ======================== Configuration ========================

const char* MQTT_CMD_TOPIC = "opengate/cmd";
const char* MQTT_ACK_TOPIC = "opengate/ack";

const int RELAY_PIN = 26;
const unsigned long PULSE_MS = 1000;

const int SERVO_PIN = 25;
const int SERVO_ANGLE = 90;
const unsigned long SERVO_MOVE_DELAY_MS = 2000;
const unsigned long SERVO_RETURN_DELAY_MS = 1000;
const int SERVO_CYCLES = 3;

// Deep sleep duration: 2 minutes in microseconds
const uint64_t DEEP_SLEEP_DURATION_US = 2ULL * 60 * 1000000;

// Command collection (phase 1). The collection loop waits up to
// MQTT_COLLECT_IDLE_MS for the first message; once something has arrived it
// keeps reading until the socket has been silent for MQTT_COLLECT_GRACE_MS, and
// never runs longer than MQTT_COLLECT_MAX_MS.
const unsigned long MQTT_COLLECT_IDLE_MS = 10000;
const unsigned long MQTT_COLLECT_GRACE_MS = 500;
const unsigned long MQTT_COLLECT_MAX_MS = 15000;

// Commands buffered during a single wake. Anything beyond this has already been
// PUBACKed by PubSubClient and is therefore lost, not deferred.
const int MAX_COLLECTED_COMMANDS = 10;

// Maximum number of processed command IDs to store
const int MAX_PROCESSED_IDS = 20;

// System time is required for certificate validity checks.
const char* NTP_SERVER = "pool.ntp.org";
const unsigned long TIME_SYNC_TIMEOUT_MS = 5000;
const time_t MIN_VALID_EPOCH = 1700000000; // 2023-11-14; only used as a sanity check

// Root CA used to validate api.telegram.org.
// The currently observed api.telegram.org certificate chain is issued by
// GoDaddy Secure Certificate Authority - G2, whose trust anchor is
// Go Daddy Root Certificate Authority - G2. Keep this certificate in the
// source tree because it is public CA material, not a secret.
static const char TELEGRAM_ROOT_CA[] PROGMEM = R"EOF(-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)EOF";

// Broker root CA (for HiveMQ Cloud: Let's Encrypt "ISRG Root X1").
// Paste the PEM certificate here — see README, "TLS certificate" section.
// If you leave the placeholder, the sketch falls back to setInsecure():
// the connection is encrypted but the server identity is NOT verified
// (vulnerable to MITM).
const char* ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// =================================================================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

WiFiClientSecure telegramClient;
UniversalTelegramBot telegramBot(TELEGRAM_BOT_TOKEN, telegramClient);

Servo servo;

Preferences nvs;
String clientId;

struct Message {
  char id[64];
  char command[32];
  bool valid;
};

// A command handler performs the hardware action for one command type and
// returns true on success. See the "Command dispatch" section for the table
// that binds handlers to command names.
typedef bool (*CommandHandler)();

struct CommandDefinition {
  const char* name;
  CommandHandler handler;
  bool requiresReservation;
};

// Commands collected during phase 1, in arrival order. The MQTT callback only
// appends here; classification and execution happen later, from setup().
Message collectedCommands[MAX_COLLECTED_COMMANDS];
volatile int collectedCount = 0;

// Commands that arrived with the buffer already full. They have been PUBACKed,
// so the broker will not re-deliver them: they are lost, and reported as such.
volatile int droppedCount = 0;

// Set by the callback on every incoming message; the collection loop uses it to
// know that the socket is still delivering and the grace period must restart.
volatile bool messageReceived = false;

// A failed publish after a long hardware action usually means the broker closed
// the socket in the meantime. One reconnection per wake is attempted to rescue
// the ACK; this flag keeps that from turning into a retry storm.
bool mqttReconnectAttempted = false;

// Telegram notifications are queued and only delivered once the MQTT connection
// has been closed, so the two TLS sessions never compete for heap.
// Worst case per wake: one recovery + one executed command + the three
// end-of-wake summaries (duplicates, unsupported, dropped).
const int MAX_QUEUED_NOTIFICATIONS = 6;

String notificationQueue[MAX_QUEUED_NOTIFICATIONS];
int notificationCount = 0;

// ============================= NVS ==============================

bool nvsInit() {
  if (!nvs.begin("opengate", false)) {
    Serial.println("[NVS] ERROR: unable to open namespace");
    return false;
  }
  return true;
}

bool hasProcessedId(const String& id) {
  String ids = nvs.getString("cmd_ids", "");
  if (ids.isEmpty()) {
    return false;
  }

  // IDs are stored as a comma-separated list. Search for a complete entry
  // to avoid false positives from partial matches.
  String list = "," + ids + ",";
  return list.indexOf("," + id + ",") != -1;
}

// Prepends the given IDs to the stored list, newest last, and truncates it to
// MAX_PROCESSED_IDS. The whole batch costs a single flash write, which matters
// when a burst of commands is acknowledged in one go.
bool saveProcessedIds(const String* newIds, int count) {
  if (count <= 0) {
    return true;
  }

  String ids = nvs.getString("cmd_ids", "");

  for (int i = 0; i < count; i++) {
    if (ids.length() > 0) {
      ids = newIds[i] + "," + ids;
    } else {
      ids = newIds[i];
    }
  }

  // Keep only the last MAX_PROCESSED_IDS by counting backwards.
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

  size_t written = nvs.putString("cmd_ids", ids);
  if (written == 0) {
    Serial.println("[NVS] ERROR: failed to save processed command ID");
    return false;
  }

  Serial.printf("[NVS] Processed IDs saved: %s\r\n", ids.c_str());
  return true;
}

bool saveProcessedId(const String& id) {
  return saveProcessedIds(&id, 1);
}

bool reservePendingId(const String& id) {
  String pending = nvs.getString("pending_id", "");

  // If a different command is already pending, do not execute another hardware action.
  if (!pending.isEmpty() && pending != id) {
    Serial.printf("[NVS] ERROR: another command is pending: %s\r\n", pending.c_str());
    return false;
  }

  if (pending == id) {
    return true;
  }

  size_t written = nvs.putString("pending_id", id);
  if (written == 0) {
    Serial.println("[NVS] ERROR: failed to persist pending command");
    return false;
  }

  // Read back to verify that the reservation is durable from our point of view.
  String verify = nvs.getString("pending_id", "");
  if (verify != id) {
    Serial.println("[NVS] ERROR: pending command verification failed");
    return false;
  }

  Serial.printf("[NVS] Command reserved as PENDING: %s\r\n", id.c_str());
  return true;
}

bool clearPendingId(const String& expectedId) {
  String pending = nvs.getString("pending_id", "");

  if (pending.isEmpty()) {
    return true;
  }

  if (pending != expectedId) {
    Serial.printf("[NVS] ERROR: pending ID mismatch (stored=%s, expected=%s)\r\n",
                  pending.c_str(), expectedId.c_str());
    return false;
  }

  if (!nvs.remove("pending_id")) {
    Serial.println("[NVS] ERROR: failed to clear pending command");
    return false;
  }

  Serial.printf("[NVS] Pending command cleared: %s\r\n", expectedId.c_str());
  return true;
}

String getPendingId() {
  return nvs.getString("pending_id", "");
}

void saveWiFiInfo() {
  if (WiFi.status() == WL_CONNECTED) {
    nvs.putString("wifi_ssid", WiFi.SSID());
    nvs.putInt("wifi_channel", WiFi.channel());
  }
}

// ===================== Message Parsing ========================

bool extractJsonString(const char* json, const char* key, char* value, int maxLen) {
  // Extracts value from JSON like: {"key":"value"}.
  // This is intentionally simple because the command schema is fixed and tiny.
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
  if (!buffer) {
    Serial.println("[MSG] ERROR: out of memory while parsing payload");
    return msg;
  }

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

void oscillateServo() {
  Serial.println("[GPIO] Starting servo oscillation");

  for (int cycle = 0; cycle < SERVO_CYCLES; cycle++) {
    Serial.printf("[GPIO] Servo cycle %d/%d\r\n", cycle + 1, SERVO_CYCLES);

    servo.write(SERVO_ANGLE);
    Serial.printf("[GPIO] Servo moved to %d degrees\r\n", SERVO_ANGLE);
    delay(SERVO_MOVE_DELAY_MS);

    servo.write(0);
    Serial.println("[GPIO] Servo returned to 0 degrees");
    delay(SERVO_RETURN_DELAY_MS);
  }

  Serial.println("[GPIO] Servo oscillation complete");
}

// ===================== Time / TLS =============================

bool syncSystemTime() {
  time_t now = time(nullptr);

  // Deep sleep preserves the ESP32 RTC/system clock. Only sync if the clock is
  // obviously invalid (e.g. after a cold boot without a previous time source).
  if (now >= MIN_VALID_EPOCH) {
    Serial.printf("[TIME] System clock already valid: %ld\r\n", (long)now);
    return true;
  }

  Serial.println("[TIME] Synchronizing system clock with NTP...");
  configTime(0, 0, NTP_SERVER);

  unsigned long start = millis();
  while (millis() - start < TIME_SYNC_TIMEOUT_MS) {
    now = time(nullptr);
    if (now >= MIN_VALID_EPOCH) {
      Serial.printf("[TIME] NTP synchronized: %ld\r\n", (long)now);
      return true;
    }
    delay(100);
  }

  Serial.println("[TIME] ERROR: NTP synchronization timed out");
  return false;
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

void onMqttMessage(char* topic, byte* payload, unsigned int length);
void connectMqtt();

String formatNotification(const String& id, const String& result) {
  if (result == "OK") {
    return "🚪 OpenGate: gate opened\nCommand ID: " + id;
  }
  if (result == "DUPLICATE") {
    return "⚠️ OpenGate: duplicate command ignored\nCommand ID: " + id;
  }
  if (result == "RECOVERED") {
    return "⚠️ OpenGate: command recovered after ESP32 reset; gate was NOT triggered again\nCommand ID: " + id;
  }
  if (result == "NVS_ERROR") {
    return "❌ OpenGate: NVS error, gate command NOT executed\nCommand ID: " + id;
  }
  if (result == "UNKNOWN_COMMAND") {
    return "⚠️ OpenGate: unsupported command received\nCommand ID: " + id;
  }
  return "⚠️ OpenGate: command result = " + result + "\nCommand ID: " + id;
}

void queueMessage(const String& message) {
  if (notificationCount >= MAX_QUEUED_NOTIFICATIONS) {
    Serial.println("[Telegram] Queue full, dropping notification");
    return;
  }

  notificationQueue[notificationCount] = message;
  notificationCount++;
}

void queueNotification(const String& id, const String& result) {
  queueMessage(formatNotification(id, result));
}

// Sends every queued notification. Call this only after mqtt.disconnect(), so
// that the MQTT mbedTLS context has been freed and the Telegram handshake has
// the whole heap available.
void flushNotifications() {
  if (notificationCount == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[Telegram] WiFi not connected, dropping %d notification(s)\r\n", notificationCount);
    notificationCount = 0;
    return;
  }

  for (int i = 0; i < notificationCount; i++) {
    bool sent = telegramBot.sendMessage(TELEGRAM_CHAT_ID, notificationQueue[i], "");
    if (sent) {
      Serial.println("[Telegram] Notification sent");
    } else {
      Serial.println("[Telegram] Failed to send notification");
    }
  }

  notificationCount = 0;
}

// Publishes a single ACK message. Returns false when MQTT is unavailable or the
// socket write fails.
bool publishAckPayload(const String& id, const String& result) {
  if (!mqtt.connected()) {
    Serial.println("[ACK] MQTT not connected, cannot publish");
    return false;
  }

  time_t now = time(nullptr);
  struct tm* timeinfo = gmtime(&now);
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", timeinfo);

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"result\":\"%s\",\"timestamp\":\"%s\"}",
           id.c_str(), result.c_str(), timestamp);

  // PubSubClient publish(topic, payload, length, retain): the library does
  // not provide QoS 1 publishing through this overload. For this ACK we keep
  // retain=false. The command itself is what uses QoS 1/persistent delivery.
  //
  // IMPORTANT: do not claim this ACK is QoS 1 if PubSubClient is configured
  // only for QoS 0 publishing.
  bool ok = mqtt.publish(MQTT_ACK_TOPIC, payload, false);

  if (ok) {
    Serial.printf("[ACK] Published (QoS 0, retain=false): %s\r\n", payload);
  } else {
    Serial.println("[ACK] Failed to publish");
  }
  return ok;
}

// Publishes the MQTT ACK for a command. Set notify=false for results that must
// not reach Telegram individually (for example each duplicate command, which is
// summarised in a single message instead).
bool publishAck(const String& id, const String& result, bool notify = true) {
  bool mqttOk = publishAckPayload(id, result);

  // A hardware action blocks for seconds, which is long enough for the broker
  // to drop the socket. Reconnect once per wake so the outcome still reaches
  // the publisher. Re-subscribing is harmless here: nothing calls mqtt.loop()
  // after this point, so anything still queued stays queued for the next wake.
  if (!mqttOk && !mqttReconnectAttempted && WiFi.status() == WL_CONNECTED) {
    mqttReconnectAttempted = true;
    Serial.println("[ACK] Publish failed; reconnecting once to retry");
    connectMqtt();
    mqttOk = publishAckPayload(id, result);
  }

  // Telegram is independent from the MQTT ACK, and is deferred until the MQTT
  // connection has been closed.
  if (notify) {
    queueNotification(id, result);
  }
  return mqttOk;
}

// MQTT message callback.
//
// Keep this function as short as possible: PubSubClient writes the QoS 1 PUBACK
// only after it returns (see PubSubClient::loop, MQTTPUBLISH branch). Commands
// are therefore only parsed and appended to collectedCommands here;
// processCollectedCommands() does the actual work once we are back in setup().
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s (%u bytes)\r\n", topic, length);

  messageReceived = true;

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

  // The PUBACK for this message is already on its way, so a command that does
  // not fit is gone for good rather than deferred to the next wake.
  if (collectedCount >= MAX_COLLECTED_COMMANDS) {
    droppedCount++;
    Serial.printf("[MQTT] WARNING: collection buffer full, command dropped: %s\r\n", msg.id);
    return;
  }

  collectedCommands[collectedCount] = msg;
  collectedCount++;
}

// ===================== Command dispatch =======================
//
// To add a new command, write a handler returning true on success and append an
// entry to COMMAND_TABLE. Nothing else needs to change: collection, parsing,
// deduplication, ACK and Telegram notification are already generic.
//
// requiresReservation marks commands with an irreversible physical effect. Those
// go through the NVS PENDING reservation, which guarantees at-most-once
// execution across a reset (see the file header), and only the first one of a
// batch is executed. Read-only or idempotent commands should set it to false:
// they are cheap to repeat, skip the two extra flash writes, and every one of
// them collected during a wake is executed.

bool handleOpen() {
  pulseRelay();
  oscillateServo();
  return true;
}

const CommandDefinition COMMAND_TABLE[] = {
  {"OPEN", handleOpen, true},
};

const int COMMAND_COUNT = sizeof(COMMAND_TABLE) / sizeof(COMMAND_TABLE[0]);

const CommandDefinition* findCommand(const String& name) {
  for (int i = 0; i < COMMAND_COUNT; i++) {
    if (name == COMMAND_TABLE[i].name) {
      return &COMMAND_TABLE[i];
    }
  }
  return nullptr;
}

// Runs one collected command and publishes its ACK. Must run outside the MQTT
// callback, and after the collection phase, so that the PUBACK for every queued
// command has already left the device before the slow work starts.
void executeCommand(const Message& message, const CommandDefinition* definition) {
  String id(message.id);
  String command(message.command);

  // Commands with no irreversible effect run directly: re-executing them after
  // a reset is harmless, so they need no PENDING reservation.
  if (!definition->requiresReservation) {
    Serial.printf("[CMD] Executing %s id=%s\r\n", command.c_str(), id.c_str());
    bool ok = definition->handler();

    if (!saveProcessedId(id)) {
      Serial.println("[CMD] WARNING: command executed but could not be committed to processed-ID list");
    }

    publishAck(id, ok ? "OK" : "FAILED");
    return;
  }

  // -----------------------------------------------------------------
  // Transactional hardware execution:
  // reserve the command in NVS BEFORE running the handler.
  // -----------------------------------------------------------------
  if (!reservePendingId(id)) {
    Serial.printf("[CMD] Refusing %s because PENDING reservation failed: %s\r\n",
                  command.c_str(), id.c_str());
    publishAck(id, "NVS_ERROR");
    return;
  }

  Serial.printf("[CMD] Executing %s id=%s\r\n", command.c_str(), id.c_str());
  bool ok = definition->handler();

  // Persist completion before acknowledging. This runs even when the handler
  // reported a failure: the hardware may have moved anyway, and at-most-once
  // forbids a second attempt.
  if (!saveProcessedId(id)) {
    // Keep pending_id in NVS. On the next boot the command will be treated as
    // already-triggered and the handler will NOT run again.
    Serial.println("[CMD] WARNING: command executed but could not be committed to processed-ID list");
    publishAck(id, "EXECUTED_NVS_ERROR");
    return;
  }

  // Once processed-ID is durable, the pending reservation can be removed.
  if (!clearPendingId(id)) {
    // processed-ID already protects against a second execution, so this is safe.
    Serial.println("[CMD] WARNING: processed ID saved but PENDING flag could not be cleared");
  }

  publishAck(id, ok ? "OK" : "FAILED");
}

// True when the same command ID already appears earlier in the collected batch,
// which happens when the broker re-delivers a message whose PUBACK was lost.
bool isRepeatedInBatch(int index, const String& id) {
  for (int i = 0; i < index; i++) {
    if (id == collectedCommands[i].id) {
      return true;
    }
  }
  return false;
}

// Phase 1: read every command the broker has queued for us. PubSubClient
// PUBACKs each message as soon as the callback returns, so after this function
// the broker considers the queue delivered and will not repeat it at the next
// wake. Nothing is executed here — commands are only buffered.
void collectQueuedCommands() {
  Serial.printf("[MQTT] Collecting queued commands (idle timeout=%lu ms)\r\n", MQTT_COLLECT_IDLE_MS);

  unsigned long start = millis();
  unsigned long lastActivity = start;

  while (millis() - start < MQTT_COLLECT_MAX_MS) {
    if (!mqtt.connected()) {
      Serial.println("[MQTT] Connection lost while collecting");
      break;
    }

    messageReceived = false;
    mqtt.loop();

    // Bytes already decrypted and waiting: another packet is on its way, so keep
    // reading without burning the grace period.
    if (messageReceived || tlsClient.available() > 0) {
      lastActivity = millis();
      continue;
    }

    // Nothing arrived yet: wait the full idle timeout. Once the first message is
    // in, a short silence is enough to declare the queue empty.
    bool anythingArrived = (collectedCount > 0 || droppedCount > 0);
    unsigned long allowedIdle = anythingArrived ? MQTT_COLLECT_GRACE_MS : MQTT_COLLECT_IDLE_MS;
    if (millis() - lastActivity >= allowedIdle) {
      break;
    }

    delay(20);
  }

  Serial.printf("[MQTT] Collection ended after %lu ms: %d collected, %d dropped\r\n",
                millis() - start, collectedCount, droppedCount);
}

// Phase 2: classify the collected commands and run at most one irreversible
// action — the first that arrived. Everything that will not run is
// acknowledged first, while the socket is still fresh, because the hardware
// action afterwards blocks for seconds and may outlive the connection.
void processCollectedCommands() {
  if (collectedCount == 0 && droppedCount == 0) {
    Serial.println("[MSG] No command received");
    return;
  }

  // ---- Pass 1: decide, without touching the hardware or the flash. ----
  // plan[i] == nullptr means "not executed"; results[i] is then its ACK result.
  const CommandDefinition* plan[MAX_COLLECTED_COMMANDS];
  String results[MAX_COLLECTED_COMMANDS];
  bool persist[MAX_COLLECTED_COMMANDS];

  bool actionSelected = false;
  int duplicates = 0;
  int unsupported = 0;

  for (int i = 0; i < collectedCount; i++) {
    String id(collectedCommands[i].id);
    String command(collectedCommands[i].command);

    plan[i] = nullptr;
    persist[i] = false;

    Serial.printf("[MSG] id=%s, command=%s\r\n", id.c_str(), command.c_str());

    // Already handled in an earlier wake, or the very same ID delivered twice.
    // Either way its ID is already on record, so nothing needs persisting.
    if (isRepeatedInBatch(i, id) || hasProcessedId(id)) {
      Serial.printf("[CMD] DUPLICATE id=%s\r\n", id.c_str());
      results[i] = "DUPLICATE";
      duplicates++;
      continue;
    }

    const CommandDefinition* definition = findCommand(command);
    if (definition == nullptr) {
      Serial.printf("[CMD] Unsupported command: %s\r\n", command.c_str());
      results[i] = "UNKNOWN_COMMAND";
      persist[i] = true;
      unsupported++;
      continue;
    }

    // Only the first irreversible command of the batch runs. A burst of OPENs
    // means the button was pressed repeatedly, not that the gate must cycle
    // once per press, so the rest count as duplicates.
    if (definition->requiresReservation && actionSelected) {
      Serial.printf("[CMD] An action is already scheduled for this wake, treating as duplicate: id=%s\r\n",
                    id.c_str());
      results[i] = "DUPLICATE";
      persist[i] = true;
      duplicates++;
      continue;
    }

    if (definition->requiresReservation) {
      actionSelected = true;
    }
    plan[i] = definition;
  }

  // ---- Pass 2: commit and acknowledge everything that will not run. ----
  // Their PUBACK has already been sent, but it may have been lost with the
  // socket; the processed-ID list is what stops a re-delivery from opening the
  // gate at a later wake.
  String skippedIds[MAX_COLLECTED_COMMANDS];
  int skippedCount = 0;
  for (int i = 0; i < collectedCount; i++) {
    if (plan[i] == nullptr && persist[i]) {
      skippedIds[skippedCount] = String(collectedCommands[i].id);
      skippedCount++;
    }
  }

  if (!saveProcessedIds(skippedIds, skippedCount)) {
    Serial.println("[CMD] WARNING: skipped commands could not be added to the processed-ID list");
  }

  // MQTT ACK only: the publisher still learns the outcome, but a burst of
  // button presses does not become a burst of Telegram messages.
  for (int i = 0; i < collectedCount; i++) {
    if (plan[i] == nullptr) {
      publishAck(String(collectedCommands[i].id), results[i], false);
    }
  }

  // ---- Pass 3: execute. ----
  for (int i = 0; i < collectedCount; i++) {
    if (plan[i] != nullptr) {
      executeCommand(collectedCommands[i], plan[i]);
    }
  }

  // ---- Pass 4: one summary per category instead of one message per command. ----
  if (duplicates > 0) {
    queueMessage("⚠️ OpenGate: " + String(duplicates) +
                 " duplicate command(s) acknowledged and ignored");
  }
  if (unsupported > 0) {
    queueMessage("⚠️ OpenGate: " + String(unsupported) +
                 " unsupported command(s) acknowledged and ignored");
  }
  if (droppedCount > 0) {
    queueMessage("❌ OpenGate: " + String(droppedCount) +
                 " command(s) lost, more than " + String(MAX_COLLECTED_COMMANDS) +
                 " arrived in a single wake");
  }
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

  unsigned long timeout = millis() + 10000;
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

void setupTls() {
  // MQTT TLS: full certificate verification using the CA configured in secrets.h.
  if (strstr(ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    tlsClient.setCACert(ROOT_CA);
  } else {
    Serial.println("[TLS/MQTT] ERROR: ROOT_CA is missing");
    tlsClient.setInsecure();
  }
  tlsClient.setTimeout(5000);

  // Telegram TLS: certificate verification using the GoDaddy Root G2 CA.
  // api.telegram.org is currently served with a GoDaddy-issued certificate,
  // and GoDaddy publishes this root certificate in its official repository.
  if (strstr(TELEGRAM_ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    telegramClient.setCACert(TELEGRAM_ROOT_CA);
    Serial.println("[TLS/Telegram] Certificate verification enabled");
  } else {
    Serial.println("[TLS/Telegram] ERROR: TELEGRAM_ROOT_CA is missing");
  }
  telegramClient.setTimeout(5000);
}

void connectMqtt() {
  int attempts = 0;
  const int MAX_ATTEMPTS = 3;

  while (!mqtt.connected() && attempts < MAX_ATTEMPTS) {
    Serial.printf("[MQTT] Connecting (attempt %d/%d)...\r\n", attempts + 1, MAX_ATTEMPTS);

    // MQTT 3.1.1 persistent session: cleanSession=false.
    // This preserves the subscription and queued QoS 1 messages while the
    // ESP32 is disconnected/deep sleeping.
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                     nullptr, 0, false, nullptr, false)) {
      Serial.println("[MQTT] Connected to broker (persistent session)");

      // Re-subscribing on an existing persistent session is redundant but
      // harmless, and it is required after the broker expires the session.
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

// ===================== Pending recovery =======================

void recoverPendingCommand() {
  String pendingId = getPendingId();
  if (pendingId.isEmpty()) {
    return;
  }

  Serial.printf("[RECOVERY] Found pending command after reset: %s\r\n", pendingId.c_str());

  // If it is already in processed_ids, the previous boot completed the hardware
  // action and persisted completion; only the final cleanup may have been interrupted.
  if (hasProcessedId(pendingId)) {
    Serial.println("[RECOVERY] Command already committed as PROCESSED; clearing stale PENDING flag");
    clearPendingId(pendingId);
    return;
  }

  // We cannot know whether the relay pulse completed before the reset. To prevent
  // a second gate opening, never execute the relay again. Instead report the state.
  Serial.println("[RECOVERY] Command execution state is uncertain; relay will NOT be triggered again");

  // Try MQTT ACK first. If MQTT is unavailable, leave PENDING in NVS and retry recovery
  // after the next boot.
  if (!mqtt.connected()) {
    Serial.println("[RECOVERY] MQTT not connected; keeping PENDING command");
    return;
  }

  bool ackOk = publishAck(pendingId, "RECOVERED");
  if (!ackOk) {
    Serial.println("[RECOVERY] MQTT ACK failed; keeping PENDING command for next boot");
    return;
  }

  if (saveProcessedId(pendingId)) {
    clearPendingId(pendingId);
    Serial.println("[RECOVERY] Pending command marked PROCESSED without re-triggering relay");
  } else {
    Serial.println("[RECOVERY] WARNING: ACK sent but processed-ID could not be saved; keeping PENDING flag");
  }
}

// ===================== Setup & Loop ============================

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  servo.attach(SERVO_PIN);
  servo.write(0);

  Serial.begin(115200);
  delay(100);

  Serial.println("\r\n================== ESP32 WAKE UP ==================");

  if (!nvsInit()) {
    Serial.println("[Init] NVS initialization failed; entering deep sleep");
    esp_deep_sleep(DEEP_SLEEP_DURATION_US);
  }

  // Generate stable client ID from MAC.
  clientId = "opengate-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[Init] Client ID: %s\r\n", clientId.c_str());

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    bool timeValid = syncSystemTime();
    if (!timeValid) {
      Serial.println("[Init] WARNING: system time is not valid; certificate verification may fail");
    }
  }

  setupTls();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);

  connectMqtt();

  if (!mqtt.connected()) {
    Serial.println("[MQTT] ERROR: Not connected to MQTT broker, skipping this wake");
  } else {
    // Recover a hardware transaction interrupted by a reset before listening for
    // new messages, so the pending command cannot be executed a second time.
    recoverPendingCommand();

    // Phase 1: read and acknowledge everything the broker has queued, so the
    // next wake starts from an empty queue.
    collectQueuedCommands();

    // Phase 2: decide and execute. mqtt.loop() is never called again from here
    // on, so no new message can arrive while the hardware is busy.
    processCollectedCommands();

    if (mqtt.connected()) {
      mqtt.disconnect();
      Serial.println("[MQTT] Disconnected");
    }
  }

  // Only now, with the MQTT TLS context freed by disconnect(), is there enough
  // heap for the handshake to api.telegram.org.
  flushNotifications();

  nvs.end();

  Serial.printf("\r\n[Sleep] Going to deep sleep for 2 minutes...\r\n");
  Serial.flush();

  esp_deep_sleep(DEEP_SLEEP_DURATION_US);
}

void loop() {
  // Never reached due to deep sleep.
}
