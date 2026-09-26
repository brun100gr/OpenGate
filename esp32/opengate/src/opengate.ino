/*
 * OpenGate — gate side (ESP32)
 *
 * Wakes from deep sleep every DEEP_SLEEP_SECONDS, connects to WiFi and MQTT and
 * collects the commands the broker has queued.
 *
 * An OPEN command does NOT move the gate. It ARMS a proximity window: the ESP32
 * stays awake for TRACKING_WINDOW_MS following the phone position published on
 * opengate/gps, and opens the gate only once the filtered position is closer
 * than GEOFENCE_RADIUS_M to the gate (GATE_LATITUDE / GATE_LONGITUDE, both in
 * secrets.h). If the phone never gets close enough the window expires and the
 * ESP32 goes back to its sleep / brief-wake cycle without touching the relay.
 *
 * OPEN_NOW is the escape hatch: it opens the gate immediately, bypassing the
 * geofence. It exists because a geofenced OPEN is useless when the location
 * permission is denied, the phone is underground, or the GPS never gets a fix.
 *
 * MQTT uses a persistent session (cleanSession=false) with a fixed client ID, so
 * QoS 1 commands published while the ESP32 was asleep are delivered on wake.
 *
 * --------------------------------------------------------------------------
 * DESIGN NOTE 1 — at-most-once hardware action
 *
 * MQTT QoS 1 is at-least-once, so the hardware transaction turns it into an
 * AT-MOST-ONCE physical action that survives a reset:
 *
 *   1. Persist the action as PENDING in NVS.
 *   2. Run the handler (relay pulse, servo, ...).
 *   3. Persist the action as PROCESSED and clear PENDING.
 *   4. Publish the ACK and queue the Telegram notification.
 *
 * If the ESP32 resets after step 1 the action is deliberately NOT executed again
 * on the next boot: without a gate-position or relay feedback signal it is
 * impossible to know whether the reset happened before or after the pulse. This
 * design prefers a missed opening over a second, unwanted one.
 *
 * The transaction key is the command ID for OPEN_NOW, but for a proximity
 * opening it is the arming command ID plus GATE_TXN_SUFFIX. The arming ID is
 * already in the processed-ID list — it was committed when OPEN armed the
 * window — so reusing it would make recoverPendingCommand() mistake an
 * interrupted pulse for a completed one.
 *
 * --------------------------------------------------------------------------
 * DESIGN NOTE 2 — the wake is split into three phases
 *
 *   1. COLLECT — every queued command is read and PUBACKed, so none of them is
 *      delivered again at the next wake. Nothing is executed, no hardware moves.
 *   2. PROCESS — the collected commands are classified, the ones that will not
 *      run are acknowledged first, and only then does anything happen: OPEN arms
 *      the proximity window, OPEN_NOW moves the hardware.
 *   3. TRACK — only when a window was armed. The position fixes are filtered and
 *      the gate is opened as soon as the phone is close enough.
 *
 * Within a single wake at most one command with an irreversible effect runs: the
 * first that arrived. Every further one is acknowledged as DUPLICATE, exactly
 * like a command whose ID had already been processed. OPEN is not irreversible —
 * re-arming an already armed window merely restarts its countdown.
 *
 * The split matters because the hardware action blocks for several seconds.
 * Executing it while commands were still unacknowledged in the broker queue left
 * the MQTT session unserviced long enough for the broker to reset the TLS
 * connection, which then lost the ACKs that had to follow.
 *
 * --------------------------------------------------------------------------
 * DESIGN NOTE 3 — PUBACK and heap timing
 *
 * PubSubClient sends the QoS 1 PUBACK only AFTER the message callback returns.
 * onMqttMessage() therefore does nothing but parse a command or a position fix
 * and buffer it; the filtering, the hardware action, the ACK and the
 * notification all run later, from the wake phases. Slow work inside the
 * callback would hold back the PUBACK, and if the socket died meanwhile the
 * broker would re-deliver the same command forever.
 *
 * Telegram notifications are deferred until after the MQTT connection is closed:
 * two concurrent mbedTLS sessions need ~40-50 KB of heap each, which is enough
 * to make the handshake fail on an ESP32. A consequence worth knowing: the ARMED
 * notification only reaches Telegram once the window is over, together with the
 * outcome.
 *
 * --------------------------------------------------------------------------
 * Adding a command: write a handler returning its ACK result code and append one
 * entry to COMMAND_TABLE (see the "Command dispatch" section). Nothing else
 * needs to change.
 *
 * Credentials and the gate coordinates live in secrets.h (gitignored): copy
 * secrets.h.example to secrets.h and fill in your values. Root CAs live in
 * certificates.h.
 *
 * Required libraries (see platformio.ini):
 *   - PubSubClient (knolleary)
 *   - UniversalTelegramBot (witnessmenow)
 *   - ESP32Servo (madhephaestus)
 *   - WiFi, WiFiClientSecure and Preferences ship with the ESP32 core.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <ESP32Servo.h>
#include <time.h>

#include "certificates.h"
#include "secrets.h"

// ======================== Configuration ========================

// --- MQTT ---
const char* MQTT_CMD_TOPIC = "opengate/cmd";
const char* MQTT_ACK_TOPIC = "opengate/ack";
const char* MQTT_GPS_TOPIC = "opengate/gps";
const int MQTT_CONNECT_ATTEMPTS = 3;
const unsigned long MQTT_RETRY_DELAY_MS = 1000;
const uint16_t MQTT_KEEPALIVE_S = 30;

// Largest command payload we are willing to parse. Payloads above PubSubClient's
// own MQTT_MAX_PACKET_SIZE never reach the callback in the first place.
const unsigned int MQTT_MAX_PAYLOAD_LEN = 512;

// --- Hardware ---
const int RELAY_PIN = 26;
const int BUILTIN_LED_PIN = 2;
const unsigned long RELAY_PULSE_MS = 1000;

const int SERVO_PIN = 25;
const int SERVO_ANGLE_REST = 0;
const int SERVO_ANGLE_ACTIVE = 90;
const unsigned long SERVO_MOVE_DELAY_MS = 2000;
const unsigned long SERVO_RETURN_DELAY_MS = 1000;
const int SERVO_CYCLES = 3;

// --- Connectivity ---
const unsigned long SERIAL_BAUD = 115200;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;
const unsigned long TLS_TIMEOUT_MS = 5000;

// System time is required for certificate validity checks.
const char* NTP_SERVER = "pool.ntp.org";
const unsigned long TIME_SYNC_TIMEOUT_MS = 5000;
const time_t MIN_VALID_EPOCH = 1700000000; // 2023-11-14; only used as a sanity check

// --- Deep sleep ---
// Deliberately NOT raised to 300 s: the phone streams its position for five
// minutes from the button press, so a five-minute sleep would routinely wake the
// ESP32 after that window had already expired, leaving it nothing to track.
const uint32_t DEEP_SLEEP_SECONDS = 120;
const uint64_t DEEP_SLEEP_DURATION_US = (uint64_t)DEEP_SLEEP_SECONDS * 1000000ULL;

// --- Command collection (phase 1) ---
// The collection loop waits up to MQTT_COLLECT_IDLE_MS for the first message;
// once something has arrived it keeps reading until the socket has been silent
// for MQTT_COLLECT_GRACE_MS, and never runs longer than MQTT_COLLECT_MAX_MS.
const unsigned long MQTT_COLLECT_IDLE_MS = 10000;
const unsigned long MQTT_COLLECT_GRACE_MS = 500;
const unsigned long MQTT_COLLECT_MAX_MS = 15000;

// --- Proximity tracking (phase 3) ---
const unsigned long TRACKING_WINDOW_MS = 300000;  // stay awake 5 min after an OPEN
const unsigned long TRACKING_POLL_DELAY_MS = 20;
const int TRACKING_RECONNECT_ATTEMPTS = 2;        // per window, before giving up

// Open the gate once the filtered position is this close to GATE_LATITUDE /
// GATE_LONGITUDE, confirmed by this many consecutive fixes. Two fixes cost one
// extra second and stop a single bad measurement from opening the gate.
const double GEOFENCE_RADIUS_M = 100.0;
const int GEOFENCE_CONFIRMATIONS = 2;

// Fixes worse than GPS_MAX_ACCURACY_M are discarded outright rather than fed to
// the filter with a huge R: a 200 m fix carries no usable information.
const double GPS_MAX_ACCURACY_M = 50.0;
const double GPS_MIN_ACCURACY_M = 3.0;      // floor, so the measurement variance is never 0
const double GPS_DEFAULT_ACCURACY_M = 15.0; // when the payload omits "accuracy"

// Nominal interval between two published fixes. dt is derived from the seq gap
// rather than from arrival time: seq is exact and immune to mobile-network
// jitter, and a gap directly expresses how many messages were lost.
// MUST mirror PUBLISH_INTERVAL_MS in the Android GpsTrackingService.
const double GPS_PUBLISH_INTERVAL_S = 1.0;

// --- Kalman filter (constant velocity) ---
// Process noise: the acceleration the motion model does not know about. 1.5
// m/s^2 covers normal driving without letting the filter chase GPS noise.
const double KALMAN_ACCEL_NOISE_MPS2 = 1.5;
// At the first fix the speed is completely unknown, so start with a very wide
// velocity variance and let the first few updates narrow it down.
const double KALMAN_INITIAL_SPEED_SIGMA_MPS = 30.0;
// A longer silence than this is treated as this long: the covariance would grow
// without bound and the prediction would be meaningless anyway.
const double KALMAN_MAX_GAP_S = 60.0;

const double EARTH_RADIUS_M = 6371000.0;

// Suffix that turns an arming command ID into the ID of the gate transaction it
// eventually triggers — see DESIGN NOTE 1.
const char* GATE_TXN_SUFFIX = "@gate";

// Commands buffered during a single wake. Anything beyond this has already been
// PUBACKed by PubSubClient and is therefore lost, not deferred.
const int MAX_COLLECTED_COMMANDS = 10;

// Maximum number of processed command IDs kept in NVS.
const int MAX_PROCESSED_IDS = 20;

// Worst case per wake: one recovery + one ARMED + the proximity outcome + the
// three end-of-wake summaries.
const int MAX_QUEUED_NOTIFICATIONS = 8;

// =========================== Types =============================
//
// These live here, ahead of every function, because the Arduino build
// auto-generates function prototypes at the top of the .ino: a type used in a
// signature must already be known at that point.

struct Message {
  char id[64];
  char command[32];
};

// One position fix decoded from opengate/gps.
struct GpsFix {
  double lat;
  double lon;
  double accuracy;      // metres, 1-sigma; GPS_DEFAULT_ACCURACY_M when absent
  uint32_t seq;         // increments once per publish attempt within a session
  char sessionId[64];   // changes on every new tracking session on the phone
};

// One axis of the constant-velocity Kalman filter, in metres from the gate. The
// 2x2 covariance is symmetric, so three scalars are enough and the whole filter
// fits in plain arithmetic — no matrix code on the ESP32.
struct AxisFilter {
  double x;    // position (m)
  double v;    // velocity (m/s)
  double p00;  // var(x)
  double p01;  // cov(x, v)
  double p11;  // var(v)
};

// What a command that arrived while the proximity window was already running
// means for that window.
enum WindowCommandOutcome {
  WINDOW_CONTINUE,  // nothing changes (duplicate, unsupported)
  WINDOW_REFRESH,   // another OPEN: restart the countdown
  WINDOW_END        // the gate was opened, there is nothing left to track
};

// Performs one command and returns the ACK result code to publish for it
// ("OK", "ARMED", "FAILED", ...). Receives the command ID because a handler may
// need to remember which command it is acting for.
typedef const char* (*CommandHandler)(const String& id);

// One entry of COMMAND_TABLE — see the "Command dispatch" section.
struct CommandDefinition {
  const char* name;
  CommandHandler handler;
  bool requiresReservation;
};

// ======================== Global state =========================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

WiFiClientSecure telegramClient;
UniversalTelegramBot telegramBot(TELEGRAM_BOT_TOKEN, telegramClient);

Servo servo;

Preferences nvs;
String clientId;

// Commands collected during phase 1, in arrival order. The MQTT callback only
// appends here; classification and execution happen later, from setup().
Message collectedCommands[MAX_COLLECTED_COMMANDS];
int collectedCount = 0;

// Commands that arrived with the buffer already full. They have been PUBACKed,
// so the broker will not re-deliver them: they are lost, and reported as such.
int droppedCount = 0;

// Set by the callback on every incoming message; the collection loop uses it to
// know that the socket is still delivering and the grace period must restart.
bool messageReceived = false;

// A failed publish after a long hardware action usually means the broker closed
// the socket in the meantime. One reconnection per wake is attempted to rescue
// the ACK; this flag keeps that from turning into a retry storm.
bool mqttReconnectAttempted = false;

// Telegram notifications are queued and only delivered once the MQTT connection
// has been closed, so the two TLS sessions never compete for heap.
String notificationQueue[MAX_QUEUED_NOTIFICATIONS];
int notificationCount = 0;

// --- Proximity tracking state ---

// Set by handleOpen(): the ID of the command that armed the proximity window.
// It becomes the root of the gate transaction ID once the gate is opened.
String armedCommandId;
bool trackingArmed = false;

// Latest fix handed over by the MQTT callback, consumed by the tracking loop.
GpsFix pendingFix;
bool pendingFixValid = false;

// The filter runs on metres east/north of the gate, one independent instance per
// axis: GPS noise is roughly isotropic and uncorrelated between the two.
AxisFilter filterEast;
AxisFilter filterNorth;
bool filterInitialised = false;
char trackedSessionId[64] = "";
uint32_t lastTrackedSeq = 0;
int insideCount = 0;

// Reported at the end of the window, so a failed opening says how close the
// phone actually got instead of just "it did not work".
double bestDistanceM = -1.0;
int gpsFixesUsed = 0;
int gpsFixesRejected = 0;

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

  // IDs are stored as a comma-separated list. Search for a complete entry to
  // avoid false positives from partial matches.
  String list = "," + ids + ",";
  return list.indexOf("," + id + ",") != -1;
}

// Prepends the given IDs to the stored list, newest first, and truncates it to
// MAX_PROCESSED_IDS. The whole batch costs a single flash write, which matters
// when a burst of commands is acknowledged in one go.
bool saveProcessedIds(const String* newIds, int count) {
  if (count <= 0) {
    return true;
  }

  String ids = nvs.getString("cmd_ids", "");

  for (int i = 0; i < count; i++) {
    ids = ids.isEmpty() ? newIds[i] : newIds[i] + "," + ids;
  }

  // Keep only the newest MAX_PROCESSED_IDS by counting separators backwards.
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

  if (nvs.putString("cmd_ids", ids) == 0) {
    Serial.println("[NVS] ERROR: failed to save processed command ID");
    return false;
  }

  Serial.printf("[NVS] Processed IDs saved: %s\r\n", ids.c_str());
  return true;
}

bool saveProcessedId(const String& id) {
  return saveProcessedIds(&id, 1);
}

String getPendingId() {
  return nvs.getString("pending_id", "");
}

bool reservePendingId(const String& id) {
  String pending = getPendingId();

  // If a different command is already pending, do not start another hardware action.
  if (!pending.isEmpty() && pending != id) {
    Serial.printf("[NVS] ERROR: another command is pending: %s\r\n", pending.c_str());
    return false;
  }

  if (pending == id) {
    return true;
  }

  if (nvs.putString("pending_id", id) == 0) {
    Serial.println("[NVS] ERROR: failed to persist pending command");
    return false;
  }

  // Read back to verify that the reservation is durable from our point of view.
  if (getPendingId() != id) {
    Serial.println("[NVS] ERROR: pending command verification failed");
    return false;
  }

  Serial.printf("[NVS] Command reserved as PENDING: %s\r\n", id.c_str());
  return true;
}

bool clearPendingId(const String& expectedId) {
  String pending = getPendingId();

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

// Caching the SSID and channel lets the next wake skip the full scan.
void saveWiFiInfo() {
  if (WiFi.status() == WL_CONNECTED) {
    nvs.putString("wifi_ssid", WiFi.SSID());
    nvs.putInt("wifi_channel", WiFi.channel());
  }
}

// ======================== Message parsing ======================

// Extracts a value from flat JSON such as {"key":"value"}. Intentionally
// minimal: the command schema is fixed and tiny, so a real parser would only
// add code size and heap pressure.
bool extractJsonString(const char* json, const char* key, char* out, size_t outSize) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

  const char* start = strstr(json, pattern);
  if (start == nullptr) {
    return false;
  }
  start += strlen(pattern);

  const char* end = strchr(start, '"');
  if (end == nullptr) {
    return false;
  }

  size_t len = end - start;
  if (len >= outSize) {
    len = outSize - 1;
  }

  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

// Fills msg from a NUL-terminated JSON payload. Returns false when a mandatory
// field is missing.
bool parseMessage(const char* json, Message& msg) {
  return extractJsonString(json, "id", msg.id, sizeof(msg.id)) &&
         extractJsonString(json, "command", msg.command, sizeof(msg.command));
}

// Extracts an unquoted value from flat JSON such as {"key":12.5}. A quoted value
// makes strtod() consume nothing, which is exactly what keeps {"id":"7f3c..."}
// from being mistaken for a number.
bool extractJsonNumber(const char* json, const char* key, double& out) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);

  const char* start = strstr(json, pattern);
  if (start == nullptr) {
    return false;
  }
  start += strlen(pattern);

  char* end = nullptr;
  double value = strtod(start, &end);
  if (end == start) {
    return false;
  }

  out = value;
  return true;
}

// Fills fix from a position payload. Only the coordinates are mandatory:
// accuracy and speed are omitted by the phone whenever the fix does not provide
// them, and a missing accuracy just means the filter has to assume one.
bool parseGpsFix(const char* json, GpsFix& fix) {
  if (!extractJsonNumber(json, "lat", fix.lat) ||
      !extractJsonNumber(json, "lon", fix.lon)) {
    return false;
  }

  if (!extractJsonNumber(json, "accuracy", fix.accuracy)) {
    fix.accuracy = GPS_DEFAULT_ACCURACY_M;
  }

  double seq = 0.0;
  fix.seq = extractJsonNumber(json, "seq", seq) ? (uint32_t)seq : 0;

  if (!extractJsonString(json, "id", fix.sessionId, sizeof(fix.sessionId))) {
    fix.sessionId[0] = '\0';
  }
  return true;
}

// =================== Geofence & Kalman filter ==================
//
// The phone publishes one fix per second over QoS 0, so some of them never
// arrive. Rather than comparing raw coordinates against the geofence, the fixes
// feed a constant-velocity Kalman filter: it smooths the GPS noise, and because
// it carries a velocity estimate it keeps predicting where the car is while
// messages are missing, instead of freezing at the last position received.
//
// Everything runs in metres east/north of the gate. Degrees would force the two
// axes to use different scales (a degree of longitude is worth less than one of
// latitude), and metres make every tuning constant directly readable.

// Equirectangular projection centred on the gate. Within a few kilometres of the
// reference point its error is far below the GPS noise, and it costs one cosine
// instead of the full geodesic machinery.
void toLocalMeters(double lat, double lon, double& east, double& north) {
  const double gateLatRad = GATE_LATITUDE * DEG_TO_RAD;
  north = EARTH_RADIUS_M * (lat - GATE_LATITUDE) * DEG_TO_RAD;
  east = EARTH_RADIUS_M * (lon - GATE_LONGITUDE) * DEG_TO_RAD * cos(gateLatRad);
}

// Seeds one axis from the first measurement: position known to within its own
// accuracy, velocity completely unknown.
void axisInit(AxisFilter& f, double z, double r) {
  f.x = z;
  f.v = 0.0;
  f.p00 = r;
  f.p01 = 0.0;
  f.p11 = KALMAN_INITIAL_SPEED_SIGMA_MPS * KALMAN_INITIAL_SPEED_SIGMA_MPS;
}

// Constant-velocity prediction over dt seconds:
//   P = F*P*F' + Q,  F = [[1, dt], [0, 1]]
// with the discrete white-noise acceleration model
//   Q = sigma_a^2 * [[dt^4/4, dt^3/2], [dt^3/2, dt^2]]
// This is where lost messages pay for themselves: a longer dt inflates the
// covariance, so the next measurement automatically weighs more.
void axisPredict(AxisFilter& f, double dt) {
  f.x += f.v * dt;

  const double p01 = f.p01;
  const double p11 = f.p11;
  f.p00 += dt * (2.0 * p01 + dt * p11);
  f.p01 += dt * p11;

  const double q = KALMAN_ACCEL_NOISE_MPS2 * KALMAN_ACCEL_NOISE_MPS2;
  const double dt2 = dt * dt;
  f.p00 += q * dt2 * dt2 / 4.0;
  f.p01 += q * dt2 * dt / 2.0;
  f.p11 += q * dt2;
}

// Measurement update for a position-only observation (H = [1 0]) with variance
// r. Note that p11 must be updated with the OLD p01.
void axisUpdate(AxisFilter& f, double z, double r) {
  const double s = f.p00 + r;
  const double k0 = f.p00 / s;
  const double k1 = f.p01 / s;
  const double y = z - f.x;

  f.x += k0 * y;
  f.v += k1 * y;

  const double p00 = f.p00;
  const double p01 = f.p01;
  f.p00 = p00 - k0 * p00;
  f.p01 = p01 - k0 * p01;
  f.p11 = f.p11 - k1 * p01;
}

// Called when a new tracking session starts: the previous velocity estimate
// describes a different trip and would poison the first predictions.
void resetFilter() {
  filterInitialised = false;
  lastTrackedSeq = 0;
  insideCount = 0;
}

double filteredDistanceM() {
  return sqrt(filterEast.x * filterEast.x + filterNorth.x * filterNorth.x);
}

// ======================== Hardware actions =====================

void initHardware() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  pinMode(BUILTIN_LED_PIN, OUTPUT);
  digitalWrite(BUILTIN_LED_PIN, LOW);

  servo.attach(SERVO_PIN);
  servo.write(SERVO_ANGLE_REST);
}

void pulseRelay() {
  Serial.println("[GPIO] Activating relay");
  digitalWrite(RELAY_PIN, HIGH);
  delay(RELAY_PULSE_MS);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("[GPIO] Relay deactivated");
}

void oscillateServo() {
  Serial.println("[GPIO] Starting servo oscillation");

  for (int cycle = 1; cycle <= SERVO_CYCLES; cycle++) {
    Serial.printf("[GPIO] Servo cycle %d/%d\r\n", cycle, SERVO_CYCLES);

    digitalWrite(BUILTIN_LED_PIN, HIGH);
    servo.write(SERVO_ANGLE_ACTIVE);
    delay(SERVO_MOVE_DELAY_MS);

    digitalWrite(BUILTIN_LED_PIN, LOW);
    servo.write(SERVO_ANGLE_REST);
    delay(SERVO_RETURN_DELAY_MS);
  }

  Serial.println("[GPIO] Servo oscillation complete");
}

// ========================== System time ========================

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

// ========================== WiFi & TLS =========================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Already connected");
    return;
  }

  String savedSsid = nvs.getString("wifi_ssid", "");
  int savedChannel = nvs.getInt("wifi_channel", -1);

  WiFi.mode(WIFI_STA);

  if (!savedSsid.isEmpty() && savedChannel > 0) {
    Serial.printf("[WiFi] Connecting to %s (cached channel %d)\r\n", savedSsid.c_str(), savedChannel);
    WiFi.begin(savedSsid.c_str(), WIFI_PASSWORD, savedChannel);
  } else if (WIFI_CHANNEL >= 0) {
    Serial.printf("[WiFi] Connecting to %s (channel %d)\r\n", WIFI_SSID, WIFI_CHANNEL);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  } else {
    Serial.printf("[WiFi] Connecting to %s\r\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
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
  // MQTT: full certificate verification. Without a usable CA the sketch still
  // connects, but over an encrypted-yet-unverified (MITM-able) channel.
  if (strstr(MQTT_ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    tlsClient.setCACert(MQTT_ROOT_CA);
  } else {
    Serial.println("[TLS/MQTT] ERROR: MQTT_ROOT_CA is missing, falling back to an unverified connection");
    tlsClient.setInsecure();
  }
  tlsClient.setTimeout(TLS_TIMEOUT_MS);

  // Telegram: verification only, no insecure fallback. A missing CA simply means
  // notifications stop working.
  if (strstr(TELEGRAM_ROOT_CA, "BEGIN CERTIFICATE") != nullptr) {
    telegramClient.setCACert(TELEGRAM_ROOT_CA);
    Serial.println("[TLS/Telegram] Certificate verification enabled");
  } else {
    Serial.println("[TLS/Telegram] ERROR: TELEGRAM_ROOT_CA is missing, notifications will fail");
  }
  telegramClient.setTimeout(TLS_TIMEOUT_MS);
}

// ======================== MQTT connection ======================

const char* mqttStateName(int state) {
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

void connectMqtt() {
  for (int attempt = 1; attempt <= MQTT_CONNECT_ATTEMPTS && !mqtt.connected(); attempt++) {
    Serial.printf("[MQTT] Connecting (attempt %d/%d)...\r\n", attempt, MQTT_CONNECT_ATTEMPTS);

    // MQTT 3.1.1 persistent session: cleanSession=false. This preserves the
    // subscription and the queued QoS 1 messages while the ESP32 is asleep.
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                     nullptr, 0, false, nullptr, false)) {
      Serial.println("[MQTT] Connected to broker (persistent session)");

      // Re-subscribing on an existing persistent session is redundant but
      // harmless, and it is required after the broker expires the session.
      if (mqtt.subscribe(MQTT_CMD_TOPIC, 1)) {
        Serial.printf("[MQTT] Subscribed to %s (QoS 1)\r\n", MQTT_CMD_TOPIC);
      } else {
        Serial.printf("[MQTT] ERROR: failed to subscribe to %s\r\n", MQTT_CMD_TOPIC);
      }
      return;
    }

    int rc = mqtt.state();
    Serial.printf("[MQTT] Connection failed (state=%d, rc=%s)\r\n", rc, mqttStateName(rc));
    if (attempt < MQTT_CONNECT_ATTEMPTS) {
      delay(MQTT_RETRY_DELAY_MS);
    }
  }

  if (!mqtt.connected()) {
    Serial.println("[MQTT] ERROR: failed to connect after all retry attempts");
  }
}

// ===================== Telegram notifications ==================

struct ResultText {
  const char* result;
  const char* text;
};

// Telegram wording per ACK result. Results missing from this table fall back to
// a generic message, so adding a result code never breaks the notification path.
const ResultText RESULT_TEXTS[] = {
  {"OK",              "🚪 OpenGate: gate opened"},
  {"ARMED",           "📡 OpenGate: command received, waiting for you to get close to the gate"},
  {"NOT_APPROACHED",  "🚪 OpenGate: nobody got close enough, gate NOT opened"},
  {"DUPLICATE",       "⚠️ OpenGate: duplicate command ignored"},
  {"RECOVERED",       "⚠️ OpenGate: command recovered after ESP32 reset; gate was NOT triggered again"},
  {"NVS_ERROR",       "❌ OpenGate: NVS error, gate command NOT executed"},
  {"UNKNOWN_COMMAND", "⚠️ OpenGate: unsupported command received"},
};

void queueMessage(const String& message) {
  if (notificationCount >= MAX_QUEUED_NOTIFICATIONS) {
    Serial.println("[Telegram] Queue full, dropping notification");
    return;
  }

  notificationQueue[notificationCount] = message;
  notificationCount++;
}

void queueNotification(const String& id, const String& result) {
  for (const ResultText& entry : RESULT_TEXTS) {
    if (result == entry.result) {
      queueMessage(String(entry.text) + "\nCommand ID: " + id);
      return;
    }
  }
  queueMessage("⚠️ OpenGate: command result = " + result + "\nCommand ID: " + id);
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
    if (telegramBot.sendMessage(TELEGRAM_CHAT_ID, notificationQueue[i], "")) {
      Serial.println("[Telegram] Notification sent");
    } else {
      Serial.println("[Telegram] Failed to send notification");
    }
  }

  notificationCount = 0;
}

// =========================== MQTT ACK ==========================

// Publishes a single ACK message. Returns false when MQTT is unavailable or the
// socket write fails.
bool publishAckPayload(const String& id, const String& result) {
  if (!mqtt.connected()) {
    Serial.println("[ACK] MQTT not connected, cannot publish");
    return false;
  }

  time_t now = time(nullptr);
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"result\":\"%s\",\"timestamp\":\"%s\"}",
           id.c_str(), result.c_str(), timestamp);

  // PubSubClient's publish(topic, payload, retain) overload is QoS 0 only; the
  // library offers no QoS 1 publishing. Only the command direction relies on
  // QoS 1 / persistent delivery, so do not document this ACK as QoS 1.
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

  // A hardware action blocks for seconds, which is long enough for the broker to
  // drop the socket. Reconnect once per wake so the outcome still reaches the
  // publisher. Re-subscribing is harmless here: nothing calls mqtt.loop() after
  // this point, so anything still queued stays queued for the next wake.
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

// ======================== MQTT callback ========================

// Keep this function as short as possible: PubSubClient writes the QoS 1 PUBACK
// only after it returns (see PubSubClient::loop, MQTTPUBLISH branch). Messages
// are therefore only parsed and buffered here; processCollectedCommands() and
// runTrackingWindow() do the actual work once we are back in the wake phases.
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  messageReceived = true;

  if (length >= MQTT_MAX_PAYLOAD_LEN) {
    Serial.printf("[MQTT] Payload too large on %s (%u bytes), ignored\r\n", topic, length);
    return;
  }

  char json[MQTT_MAX_PAYLOAD_LEN];
  memcpy(json, payload, length);
  json[length] = '\0';

  // Position fixes arrive once a second for minutes on end, so they get a single
  // compact log line instead of the full raw payload dump.
  if (strcmp(topic, MQTT_GPS_TOPIC) == 0) {
    GpsFix fix;
    if (!parseGpsFix(json, fix)) {
      Serial.println("[GPS] Malformed fix, ignored");
      return;
    }

    // Overwriting a fix the tracking loop has not consumed yet is deliberate: at
    // 1 Hz over QoS 0 only the freshest position is worth anything.
    pendingFix = fix;
    pendingFixValid = true;
    return;
  }

  Serial.printf("[MQTT] Message on %s (%u bytes)\r\n", topic, length);
  Serial.printf("[MQTT] Raw payload: %s\r\n", json);

  Message msg;
  if (!parseMessage(json, msg)) {
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

// ======================== Command dispatch =====================
//
// To add a command, write a handler returning its ACK result code and append an
// entry to COMMAND_TABLE. Collection, parsing, deduplication, ACK and Telegram
// notification are already generic.
//
// requiresReservation marks commands with an irreversible physical effect. Those
// go through the NVS PENDING reservation, which guarantees at-most-once
// execution across a reset (see DESIGN NOTE 1), and only the first one of a
// batch is executed. Read-only or idempotent commands should set it to false:
// they are cheap to repeat, skip the two extra flash writes, and every one of
// them collected during a wake is executed.

const char* handleOpen(const String& id) {
  // No hardware here. OPEN arms the proximity window; the gate is opened later,
  // by runTrackingWindow(), and only if the phone actually gets close enough.
  // Arming is idempotent, which is why this command needs no NVS reservation:
  // a second OPEN simply restarts the countdown.
  armedCommandId = id;
  trackingArmed = true;
  Serial.printf("[TRACK] Proximity window armed by %s\r\n", id.c_str());
  return "ARMED";
}

// Escape hatch for when the geofence cannot work: location permission denied,
// phone underground, GPS unable to get a fix.
const char* handleOpenNow(const String& id) {
  pulseRelay();
  oscillateServo();
  return "OK";
}

const CommandDefinition COMMAND_TABLE[] = {
  {"OPEN",     handleOpen,    false},  // arms the proximity window, nothing moves
  {"OPEN_NOW", handleOpenNow, true},   // opens immediately, bypassing the geofence
};

const CommandDefinition* findCommand(const char* name) {
  for (const CommandDefinition& definition : COMMAND_TABLE) {
    if (strcmp(name, definition.name) == 0) {
      return &definition;
    }
  }
  return nullptr;
}

// Runs one collected command and publishes its ACK. Must run outside the MQTT
// callback, and after the collection phase, so that the PUBACK for every queued
// command has already left the device before the slow work starts.
void executeCommand(const Message& message, const CommandDefinition* definition) {
  const String id(message.id);
  const bool reserved = definition->requiresReservation;

  // Transactional hardware execution: reserve the command in NVS BEFORE running
  // the handler, so a reset mid-action cannot lead to a second execution.
  if (reserved && !reservePendingId(id)) {
    Serial.printf("[CMD] Refusing %s because PENDING reservation failed: %s\r\n",
                  message.command, id.c_str());
    publishAck(id, "NVS_ERROR");
    return;
  }

  Serial.printf("[CMD] Executing %s id=%s\r\n", message.command, id.c_str());
  const char* result = definition->handler(id);

  // Persist completion before acknowledging. This runs even when the handler
  // reported a failure: the hardware may have moved anyway, and at-most-once
  // forbids a second attempt.
  if (!saveProcessedId(id)) {
    // pending_id is deliberately left in NVS: on the next boot the command is
    // treated as already-triggered and the handler will NOT run again.
    Serial.println("[CMD] WARNING: command executed but could not be committed to the processed-ID list");
    publishAck(id, reserved ? "EXECUTED_NVS_ERROR" : result);
    return;
  }

  // Once the processed ID is durable, the pending reservation can be removed.
  if (reserved && !clearPendingId(id)) {
    // The processed ID already protects against a second execution, so this is safe.
    Serial.println("[CMD] WARNING: processed ID saved but PENDING flag could not be cleared");
  }

  publishAck(id, result);
}

// ========================== Wake phases ========================

// Phase 1: read every command the broker has queued for us. PubSubClient PUBACKs
// each message as soon as the callback returns, so after this function the broker
// considers the queue delivered and will not repeat it at the next wake. Nothing
// is executed here — commands are only buffered.
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

    // Bytes already decrypted and waiting mean another packet is on its way, so
    // keep reading without burning the grace period.
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

// True when the same command ID already appears earlier in the collected batch,
// which happens when the broker re-delivers a message whose PUBACK was lost.
bool isRepeatedInBatch(int index, const char* id) {
  for (int i = 0; i < index; i++) {
    if (strcmp(id, collectedCommands[i].id) == 0) {
      return true;
    }
  }
  return false;
}

// What pass 1 of processCollectedCommands() decided for one collected command.
struct CommandPlan {
  const CommandDefinition* definition; // nullptr => the command will not run
  const char* result;                  // ACK result when definition == nullptr
  bool persistId;                      // add the ID to the processed-ID list
};

// Phase 2: classify the collected commands and run at most one irreversible
// action — the first that arrived. Everything that will not run is acknowledged
// first, while the socket is still fresh, because the hardware action afterwards
// blocks for seconds and may outlive the connection.
//
// OPEN no longer counts as irreversible: it only arms the proximity window, so a
// burst of presses re-arms it instead of being collapsed into a single action.
void processCollectedCommands() {
  if (collectedCount == 0 && droppedCount == 0) {
    Serial.println("[MSG] No command received");
    return;
  }

  CommandPlan plans[MAX_COLLECTED_COMMANDS] = {};
  bool actionSelected = false;
  int duplicates = 0;
  int unsupported = 0;

  // ---- Pass 1: decide, without touching the hardware or the flash. ----
  for (int i = 0; i < collectedCount; i++) {
    const Message& msg = collectedCommands[i];
    Serial.printf("[MSG] id=%s, command=%s\r\n", msg.id, msg.command);

    // Already handled in an earlier wake, or the very same ID delivered twice.
    // Either way its ID is already on record, so nothing needs persisting.
    if (isRepeatedInBatch(i, msg.id) || hasProcessedId(msg.id)) {
      Serial.printf("[CMD] DUPLICATE id=%s\r\n", msg.id);
      plans[i].result = "DUPLICATE";
      duplicates++;
      continue;
    }

    const CommandDefinition* definition = findCommand(msg.command);
    if (definition == nullptr) {
      Serial.printf("[CMD] Unsupported command: %s\r\n", msg.command);
      plans[i].result = "UNKNOWN_COMMAND";
      plans[i].persistId = true;
      unsupported++;
      continue;
    }

    if (definition->requiresReservation) {
      // Only the first irreversible command of the batch runs. A burst of OPENs
      // means the button was pressed repeatedly, not that the gate must cycle
      // once per press, so the rest count as duplicates.
      if (actionSelected) {
        Serial.printf("[CMD] An action is already scheduled for this wake, treating as duplicate: id=%s\r\n",
                      msg.id);
        plans[i].result = "DUPLICATE";
        plans[i].persistId = true;
        duplicates++;
        continue;
      }
      actionSelected = true;
    }

    plans[i].definition = definition;
  }

  // ---- Pass 2: commit and acknowledge everything that will not run. ----
  // Their PUBACK has already been sent, but it may have been lost with the
  // socket; the processed-ID list is what stops a re-delivery from opening the
  // gate at a later wake.
  String skippedIds[MAX_COLLECTED_COMMANDS];
  int skippedCount = 0;
  for (int i = 0; i < collectedCount; i++) {
    if (plans[i].definition == nullptr && plans[i].persistId) {
      skippedIds[skippedCount] = collectedCommands[i].id;
      skippedCount++;
    }
  }

  if (!saveProcessedIds(skippedIds, skippedCount)) {
    Serial.println("[CMD] WARNING: skipped commands could not be added to the processed-ID list");
  }

  // MQTT ACK only: the publisher still learns the outcome, but a burst of button
  // presses does not become a burst of Telegram messages.
  for (int i = 0; i < collectedCount; i++) {
    if (plans[i].definition == nullptr) {
      publishAck(collectedCommands[i].id, plans[i].result, false);
    }
  }

  // ---- Pass 3: execute. ----
  for (int i = 0; i < collectedCount; i++) {
    if (plans[i].definition != nullptr) {
      executeCommand(collectedCommands[i], plans[i].definition);
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

// The gate transaction the proximity trigger commits, derived from the command
// that armed the window. See DESIGN NOTE 1 for why it cannot be the bare command
// ID: that one is already in the processed-ID list.
String gateTransactionId(const String& commandId) {
  return commandId + GATE_TXN_SUFFIX;
}

// Opens the gate because the phone reached the geofence. Same at-most-once
// transaction as executeCommand(), on the gate transaction ID.
void openGateOnApproach(double distance) {
  const String txnId = gateTransactionId(armedCommandId);

  if (!reservePendingId(txnId)) {
    Serial.println("[TRACK] Refusing to open: PENDING reservation failed");
    publishAck(armedCommandId, "NVS_ERROR");
    return;
  }

  Serial.printf("[TRACK] Phone is %.0f m away, opening the gate\r\n", distance);
  pulseRelay();
  oscillateServo();

  if (!saveProcessedId(txnId)) {
    Serial.println("[TRACK] WARNING: gate opened but the transaction could not be committed");
    publishAck(armedCommandId, "EXECUTED_NVS_ERROR");
    return;
  }

  if (!clearPendingId(txnId)) {
    Serial.println("[TRACK] WARNING: transaction committed but PENDING flag could not be cleared");
  }

  // The generic Telegram text for OK says nothing about the approach, so the
  // notification is queued by hand with the distance that triggered it.
  publishAck(armedCommandId, "OK", false);
  queueMessage("🚪 OpenGate: gate opened on approach (" + String(distance, 0) +
               " m)\nCommand ID: " + armedCommandId);
}

// Feeds one fix to the filter and reports whether the gate should now open.
bool consumeFix(const GpsFix& fix) {
  if (fix.accuracy > GPS_MAX_ACCURACY_M) {
    gpsFixesRejected++;
    Serial.printf("[GPS] seq=%u discarded, accuracy %.0f m is worse than %.0f m\r\n",
                  fix.seq, fix.accuracy, GPS_MAX_ACCURACY_M);
    return false;
  }

  // A different session ID means the phone started a new tracking session: the
  // velocity estimate belongs to a previous trip and seq restarted from zero.
  if (strcmp(fix.sessionId, trackedSessionId) != 0) {
    Serial.printf("[GPS] New tracking session %s\r\n", fix.sessionId);
    strncpy(trackedSessionId, fix.sessionId, sizeof(trackedSessionId) - 1);
    trackedSessionId[sizeof(trackedSessionId) - 1] = '\0';
    resetFilter();
  } else if (filterInitialised && fix.seq <= lastTrackedSeq) {
    // QoS 0 loses messages, it does not reorder them, but a repeated or stale
    // fix would still corrupt dt and drag the velocity estimate backwards.
    Serial.printf("[GPS] seq=%u is not newer than %u, ignored\r\n", fix.seq, lastTrackedSeq);
    return false;
  }

  double east = 0.0;
  double north = 0.0;
  toLocalMeters(fix.lat, fix.lon, east, north);

  double sigma = fix.accuracy < GPS_MIN_ACCURACY_M ? GPS_MIN_ACCURACY_M : fix.accuracy;
  const double r = sigma * sigma;

  if (!filterInitialised) {
    axisInit(filterEast, east, r);
    axisInit(filterNorth, north, r);
    filterInitialised = true;
  } else {
    // dt comes from the seq gap, not from arrival time: seq is exact, immune to
    // network jitter, and a gap of n is precisely n lost messages.
    double dt = (double)(fix.seq - lastTrackedSeq) * GPS_PUBLISH_INTERVAL_S;
    if (dt > KALMAN_MAX_GAP_S) {
      dt = KALMAN_MAX_GAP_S;
    }

    axisPredict(filterEast, dt);
    axisPredict(filterNorth, dt);
    axisUpdate(filterEast, east, r);
    axisUpdate(filterNorth, north, r);
  }

  lastTrackedSeq = fix.seq;
  gpsFixesUsed++;

  const double distance = filteredDistanceM();
  if (bestDistanceM < 0.0 || distance < bestDistanceM) {
    bestDistanceM = distance;
  }

  // Requiring GEOFENCE_CONFIRMATIONS consecutive fixes inside the radius costs
  // about a second and stops one bad measurement from opening the gate.
  if (distance <= GEOFENCE_RADIUS_M) {
    insideCount++;
  } else {
    insideCount = 0;
  }

  Serial.printf("[GPS] seq=%u lat=%.6f lon=%.6f acc=%.0fm -> filtered distance %.0f m (inside %d/%d)\r\n",
                fix.seq, fix.lat, fix.lon, fix.accuracy, distance, insideCount, GEOFENCE_CONFIRMATIONS);

  return insideCount >= GEOFENCE_CONFIRMATIONS;
}

// A command that arrives while the window is already running was buffered by the
// callback exactly as in phase 1, but phase 2 is long over. Classifying it here
// rather than after the window keeps the ACK useful.
WindowCommandOutcome handleCommandDuringWindow(const Message& message) {
  const String id(message.id);
  Serial.printf("[TRACK] Command during window: id=%s, command=%s\r\n", message.id, message.command);

  if (hasProcessedId(id)) {
    publishAck(id, "DUPLICATE", false);
    return WINDOW_CONTINUE;
  }

  const CommandDefinition* definition = findCommand(message.command);
  if (definition == nullptr) {
    saveProcessedId(id);
    publishAck(id, "UNKNOWN_COMMAND", false);
    return WINDOW_CONTINUE;
  }

  executeCommand(message, definition);

  // The only commands that reserve are the ones that move the hardware, and with
  // the gate already open there is nothing left to track. Anything else is an
  // OPEN, which re-armed the window under a new ID: restart the countdown.
  return definition->requiresReservation ? WINDOW_END : WINDOW_REFRESH;
}

// Phase 3: stay awake following the phone until it reaches the geofence or the
// window expires. Reached only when phase 2 armed a window.
void runTrackingWindow() {
  // QoS 0 on purpose. A persistent session never queues QoS 0 messages for an
  // offline client, so this subscription cannot flood the ESP32 with stale
  // positions at the next wake; the unsubscribe at the end keeps the sleeping
  // session limited to the command topic anyway.
  if (mqtt.subscribe(MQTT_GPS_TOPIC, 0)) {
    Serial.printf("[TRACK] Subscribed to %s (QoS 0)\r\n", MQTT_GPS_TOPIC);
  } else {
    Serial.printf("[TRACK] ERROR: failed to subscribe to %s\r\n", MQTT_GPS_TOPIC);
  }

  Serial.printf("[TRACK] Waiting up to %lu s for the phone to get within %.0f m\r\n",
                TRACKING_WINDOW_MS / 1000, GEOFENCE_RADIUS_M);

  unsigned long windowStart = millis();
  int handledCommands = collectedCount;  // everything before this ran in phase 2
  int reconnects = 0;
  bool opened = false;
  bool aborted = false;

  while (millis() - windowStart < TRACKING_WINDOW_MS) {
    if (!mqtt.connected()) {
      if (reconnects >= TRACKING_RECONNECT_ATTEMPTS) {
        Serial.println("[TRACK] Connection lost for good, abandoning the window");
        aborted = true;
        break;
      }
      reconnects++;
      Serial.printf("[TRACK] Connection lost, reconnecting (%d/%d)\r\n",
                    reconnects, TRACKING_RECONNECT_ATTEMPTS);
      connectMqtt();
      if (mqtt.connected()) {
        mqtt.subscribe(MQTT_GPS_TOPIC, 0);
      }
      continue;
    }

    mqtt.loop();

    bool ended = false;
    while (handledCommands < collectedCount) {
      const WindowCommandOutcome outcome = handleCommandDuringWindow(collectedCommands[handledCommands]);
      handledCommands++;
      if (outcome == WINDOW_REFRESH) {
        windowStart = millis();
        Serial.println("[TRACK] Window countdown restarted");
      } else if (outcome == WINDOW_END) {
        opened = true;
        ended = true;
      }
    }
    if (ended) {
      break;
    }

    if (pendingFixValid) {
      pendingFixValid = false;
      if (consumeFix(pendingFix)) {
        openGateOnApproach(filteredDistanceM());
        opened = true;
        break;
      }
    }

    delay(TRACKING_POLL_DELAY_MS);
  }

  if (mqtt.connected()) {
    mqtt.unsubscribe(MQTT_GPS_TOPIC);
  }

  Serial.printf("[TRACK] Window ended after %lu s: %d fix(es) used, %d rejected\r\n",
                (millis() - windowStart) / 1000, gpsFixesUsed, gpsFixesRejected);

  if (opened || aborted) {
    return;
  }

  // The generic Telegram text cannot say how close the phone got, which is the
  // one detail that makes a failed opening diagnosable.
  publishAck(armedCommandId, "NOT_APPROACHED", false);
  if (bestDistanceM < 0.0) {
    queueMessage("🚪 OpenGate: no position received, gate NOT opened\nCommand ID: " + armedCommandId);
  } else {
    queueMessage("🚪 OpenGate: closest approach was " + String(bestDistanceM, 0) +
                 " m, more than the " + String(GEOFENCE_RADIUS_M, 0) +
                 " m needed; gate NOT opened\nCommand ID: " + armedCommandId);
  }
}

// Runs before phase 1, so a hardware transaction interrupted by a reset can
// never be executed a second time by a command that arrives now.
void recoverPendingCommand() {
  String pendingId = getPendingId();
  if (pendingId.isEmpty()) {
    return;
  }

  Serial.printf("[RECOVERY] Found pending transaction after reset: %s\r\n", pendingId.c_str());

  // NVS keys the transaction, but the publisher only knows the command ID it
  // sent, so the gate suffix is stripped before the ACK goes out.
  String ackId = pendingId;
  if (ackId.endsWith(GATE_TXN_SUFFIX)) {
    ackId = ackId.substring(0, ackId.length() - strlen(GATE_TXN_SUFFIX));
  }

  // If it is already in the processed-ID list, the previous boot completed the
  // hardware action and persisted completion; only the final cleanup was
  // interrupted.
  if (hasProcessedId(pendingId)) {
    Serial.println("[RECOVERY] Transaction already committed as PROCESSED; clearing stale PENDING flag");
    clearPendingId(pendingId);
    return;
  }

  // We cannot know whether the relay pulse completed before the reset. To prevent
  // a second gate opening, never run the handler again — just report the state.
  Serial.println("[RECOVERY] Execution state is uncertain; relay will NOT be triggered again");

  // If the outcome cannot be reported, leave PENDING in NVS and retry recovery
  // at the next boot.
  if (!publishAck(ackId, "RECOVERED")) {
    Serial.println("[RECOVERY] MQTT ACK failed; keeping PENDING transaction for next boot");
    return;
  }

  if (saveProcessedId(pendingId)) {
    clearPendingId(pendingId);
    Serial.println("[RECOVERY] Pending transaction marked PROCESSED without re-triggering relay");
  } else {
    Serial.println("[RECOVERY] WARNING: ACK sent but processed ID could not be saved; keeping PENDING flag");
  }
}

// ========================= Setup & loop ========================

void enterDeepSleep() {
  Serial.printf("\r\n[Sleep] Going to deep sleep for %u s...\r\n", DEEP_SLEEP_SECONDS);
  Serial.flush();
  esp_deep_sleep(DEEP_SLEEP_DURATION_US);
}

// The whole MQTT part of a wake, from recovery to disconnection.
void runWakeCycle() {
  recoverPendingCommand();

  // Phase 1: read and acknowledge everything the broker has queued, so the next
  // wake starts from an empty queue.
  collectQueuedCommands();

  // Phase 2: decide, arm, and run at most one hardware action.
  processCollectedCommands();

  // Phase 3: an OPEN did not move anything, it armed a proximity window. Stay
  // awake following the phone until it is close enough, or until time runs out.
  // Unlike phase 2 this does service mqtt.loop(), so commands arriving now are
  // handled inside the window instead of being silently buffered.
  if (trackingArmed) {
    runTrackingWindow();
  }

  if (mqtt.connected()) {
    mqtt.disconnect();
    Serial.println("[MQTT] Disconnected");
  }
}

void setup() {
  initHardware();

  Serial.begin(SERIAL_BAUD);
  delay(100);
  Serial.println("\r\n================== ESP32 WAKE UP ==================");

  if (!nvsInit()) {
    Serial.println("[Init] NVS initialization failed; entering deep sleep");
    enterDeepSleep();
  }

  // Stable client ID derived from the MAC, so the broker recognises the same
  // persistent session at every wake.
  clientId = "opengate-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[Init] Client ID: %s\r\n", clientId.c_str());

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED && !syncSystemTime()) {
    Serial.println("[Init] WARNING: system time is not valid; certificate verification may fail");
  }

  setupTls();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  connectMqtt();

  if (mqtt.connected()) {
    runWakeCycle();
  } else {
    Serial.println("[MQTT] ERROR: not connected to the broker, skipping this wake");
  }

  // Only now, with the MQTT TLS context freed by disconnect(), is there enough
  // heap for the handshake to api.telegram.org.
  flushNotifications();

  nvs.end();
  enterDeepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
