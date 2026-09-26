# OpenGate — MQTT Debug Commands

Documentation of `mosquitto_pub` and `mosquitto_sub` commands to emulate ESP32 and Android application without running them simultaneously.

## MQTT Architecture

- **Broker**: configured in `secrets.h`
- **Command topic**: `opengate/cmd` (QoS 1, persistent session)
- **ACK topic**: `opengate/ack` (QoS 0)
- **GPS topic**: `opengate/gps` (QoS 0, published by the Android app, read by the
  ESP32 only while a proximity window is open)
- **Command format**: `{"id":"<uuid>","command":"<command-name>"}`
- **ACK format**: `{"id":"<uuid>","result":"<result-code>","timestamp":"<iso-timestamp>"}`
- **GPS format**: `{"id":"<session-uuid>","seq":<n>,"lat":<deg>,"lon":<deg>,"accuracy":<m>,"speed":<m/s>,"timestamp":"<iso-timestamp>"}`

## Supported Commands

| Command | Description | Result Codes |
|---------|-------------|-----------------|
| `OPEN` | Arm a five-minute proximity window — nothing moves yet | `ARMED`, `DUPLICATE` |
| `OPEN_NOW` | Pulse relay + oscillate servo immediately, skipping the geofence | `OK`, `DUPLICATE`, `RECOVERED`, `NVS_ERROR`, `FAILED` |

`OPEN` no longer opens the gate on its own. It is acknowledged with `ARMED` and
the ESP32 then stays awake for five minutes watching `opengate/gps`. The gate
opens — with a second ACK carrying the *same* command ID and `result` `OK` —
only once the filtered phone position comes within 100 m of the gate. If that
never happens the window closes with a `NOT_APPROACHED` ACK and the board goes
back to sleep.

`OPEN_NOW` is the escape hatch for when the phone has no fix or location
permission was denied: it behaves exactly like the old `OPEN` did.

| Extra result code | Meaning |
|---|---|
| `ARMED` | Proximity window open, waiting for the phone to get close |
| `NOT_APPROACHED` | Window expired, nobody came within 100 m, gate NOT opened |

## ESP32 Emulation

The ESP32 subscribes to `opengate/cmd` and publishes ACKs to `opengate/ack`.

### Subscribe to Commands

```bash
mosquitto_sub \
  -h <broker-host> \
  -u <mqtt-user> \
  -P <mqtt-password> \
  --cafile <ca-cert> \
  -t opengate/cmd \
  -q 1
```

**With HiveMQ Cloud (test):**
```bash
mosquitto_sub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1
```

### Publish Manual ACK

When you receive a command via `mosquitto_sub`, respond by publishing an ACK:

**Received Command:**
```json
{"id":"cmd-001","command":"OPEN"}
```

**ACK to Publish:**
```bash
mosquitto_pub \
  -h <broker-host> \
  -u <mqtt-user> \
  -P <mqtt-password> \
  --cafile <ca-cert> \
  -t opengate/ack \
  -m '{"id":"cmd-001","result":"OK","timestamp":"2026-09-19T10:30:45Z"}'
```

**With HiveMQ Cloud (test):**
```bash
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/ack \
  -m '{"id":"cmd-001","result":"OK","timestamp":"2026-09-19T10:30:45Z"}'
```

---

## Android Application Emulation

The Android app publishes commands to `opengate/cmd` and subscribes to `opengate/ack`.

### Publish a Command

```bash
mosquitto_pub \
  -h <broker-host> \
  -u <mqtt-user> \
  -P <mqtt-password> \
  --cafile <ca-cert> \
  -t opengate/cmd \
  -q 1 \
  -m '{"id":"cmd-001","command":"OPEN"}'
```

**With HiveMQ Cloud (test):**
```bash
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1 \
  -m '{"id":"cmd-001","command":"OPEN"}'
```

### Subscribe to ACKs

```bash
mosquitto_sub \
  -h <broker-host> \
  -u <mqtt-user> \
  -P <mqtt-password> \
  --cafile <ca-cert> \
  -t opengate/ack
```

**With HiveMQ Cloud (test):**
```bash
mosquitto_sub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/ack
```

### Watch the GPS Stream

After the "Open gate" button is pressed, the app publishes its position once a
second for five minutes. Nothing answers on this topic: it is a one-way stream.

```bash
mosquitto_sub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/gps
```

Expected output, one line per second:

```json
{"id":"7f3c...","seq":0,"lat":45.123456,"lon":9.123456,"accuracy":8.5,"speed":12.3,"timestamp":"2026-09-22T12:00:00.123Z"}
{"id":"7f3c...","seq":1,"lat":45.123461,"lon":9.123470,"accuracy":8.5,"speed":12.4,"timestamp":"2026-09-22T12:00:01.124Z"}
```

`id` identifies the tracking session, `seq` increments on every publish attempt:
gaps in `seq` are messages lost on the way, which QoS 0 allows by design.
`accuracy` and `speed` only appear when the fix provides them.

The ESP32 subscribes to this topic too, but only between an `OPEN` and the end
of the five-minute window — subscribing at QoS 0 means the broker never queues
positions for it while it sleeps, so it can never wake up to a flood of stale
coordinates.

---

## Debug Scenarios

### Scenario 1: Test Complete Command Flow

**Terminal 1 (simulate ESP32):**
```bash
# Listen for commands
mosquitto_sub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1
```

**Terminal 2 (simulate Android):**
```bash
# Send a command
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1 \
  -m '{"id":"cmd-001","command":"OPEN"}'

# Listen for ACKs
mosquitto_sub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/ack
```

**Terminal 1 (after receiving command):**
```bash
# Respond with ACK
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/ack \
  -m '{"id":"cmd-001","result":"OK","timestamp":"2026-09-19T10:30:45Z"}'
```

### Scenario 2: Test Duplicates

Send the same command twice (same ID) from Terminal 2. Terminal 1 should receive it twice.

```bash
# Terminal 1: receives both original command and duplicate

# Terminal 2: send same ID
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1 \
  -m '{"id":"cmd-001","command":"OPEN"}'
```

The ESP32 should recognize the duplicate and publish an ACK with `"result":"DUPLICATE"`.

### Scenario 3: Test Unsupported Commands

```bash
# Terminal 2: send an unknown command
mosquitto_pub \
  -h <region>.hivemq.cloud \
  -p 8883 \
  -u <username> \
  -P <password> \
  -t opengate/cmd \
  -q 1 \
  -m '{"id":"cmd-002","command":"INVALID_CMD"}'

# Terminal 1 should respond with:
# {"id":"cmd-002","result":"UNKNOWN_COMMAND",...}
```

### Scenario 4: Simulate an Approach (proximity opening)

Opens the gate without a phone, by hand-feeding positions that close in on the
coordinates in `secrets.h`. The values below assume the placeholder gate at
`45.123456, 9.123456` — recompute them against your own `GATE_LATITUDE` /
`GATE_LONGITUDE`, otherwise every fix lands kilometres away and nothing opens.

```bash
# Terminal 1: watch the ACKs
mosquitto_sub -h <region>.hivemq.cloud -p 8883 -u <username> -P <password> \
  -t opengate/ack

# Terminal 2: arm the window. The ESP32 answers ARMED and stays awake 5 minutes.
mosquitto_pub -h <region>.hivemq.cloud -p 8883 -u <username> -P <password> \
  -t opengate/cmd -q 1 -m '{"id":"cmd-010","command":"OPEN"}'

# Terminal 2: walk in. seq jumps by 10, so the sketch reads each step as 10 s
# apart — roughly 15 m/s, a plausible car. Send them a few seconds apart.
for f in \
  '{"id":"sim-1","seq":10,"lat":45.127053,"lon":9.123456,"accuracy":8.0}' \
  '{"id":"sim-1","seq":20,"lat":45.125704,"lon":9.123456,"accuracy":8.0}' \
  '{"id":"sim-1","seq":30,"lat":45.124535,"lon":9.123456,"accuracy":8.0}' \
  '{"id":"sim-1","seq":40,"lat":45.123996,"lon":9.123456,"accuracy":8.0}' \
  '{"id":"sim-1","seq":50,"lat":45.123726,"lon":9.123456,"accuracy":8.0}' ; do
    mosquitto_pub -h <region>.hivemq.cloud -p 8883 -u <username> -P <password> \
      -t opengate/gps -m "$f"
    sleep 2
done
```

Those five fixes sit about 400, 250, 120, 60 and 30 m north of the gate. The
first two only teach the filter how fast you are moving; the relay fires on the
last one, when the *filtered* distance has been under 100 m for two fixes in a
row. Terminal 1 shows `ARMED` first, then `OK` on the same `cmd-010` ID.

Things that make this scenario fail on purpose, all worth trying:

- Drop `"accuracy"` above 50 m — the fix is rejected as too noisy and ignored.
- Reuse a `seq` already sent — rejected as out of order.
- Change `"id"` mid-run — treated as a new tracking session and the filter
  restarts from scratch, so the confirmation count goes back to zero.
- Send nothing at all and wait out the five minutes — `NOT_APPROACHED`.

---

## Debug Notes

1. **Unique UUIDs**: make sure to use unique IDs for each command. Use a UUID or simply incrementals (`cmd-001`, `cmd-002`, etc.)

2. **ISO8601 Timestamp**: when responding manually with ACK, use a timestamp in `YYYY-MM-DDTHH:MM:SSZ` format

3. **QoS 1 for Commands**: commands use QoS 1 to guarantee delivery, but ACKs use QoS 0

4. **Persistent Session**: the ESP32 uses persistent session, so commands remain in queue even if the client is offline

5. **Offline Testing**: you can test the Android app without the ESP32 active:
   - Publish a command with `mosquitto_pub`
   - The broker will queue it (persistent session)
   - The ESP32 will receive it on next wake

---

## Environment Variables (for convenience)

Create a local `.env.mqtt` file with your credentials:

```bash
export MQTT_HOST="your-region.hivemq.cloud"
export MQTT_PORT=8883
export MQTT_USER="your-username"
export MQTT_PASSWORD="your-password"
```

Then use the aliases:

```bash
# ESP32 subscriber
alias esp32-listen="mosquitto_sub -h $MQTT_HOST -p $MQTT_PORT -u $MQTT_USER -P $MQTT_PASSWORD -t opengate/cmd -q 1"

# Android publisher
alias android-open="mosquitto_pub -h $MQTT_HOST -p $MQTT_PORT -u $MQTT_USER -P $MQTT_PASSWORD -t opengate/cmd -q 1 -m '{\"id\":\"cmd-001\",\"command\":\"OPEN\"}'"

# ACK listener
alias ack-listen="mosquitto_sub -h $MQTT_HOST -p $MQTT_PORT -u $MQTT_USER -P $MQTT_PASSWORD -t opengate/ack"

# ACK publisher
alias esp32-ack="mosquitto_pub -h $MQTT_HOST -p $MQTT_PORT -u $MQTT_USER -P $MQTT_PASSWORD -t opengate/ack -m '{\"id\":\"cmd-001\",\"result\":\"OK\",\"timestamp\":\"2026-09-19T10:30:45Z\"}'"
```

Sourcing:
```bash
source .env.mqtt
```

Usage:
```bash
esp32-listen        # in Terminal 1
android-open        # in Terminal 2
esp32-ack          # in Terminal 1 after receiving command
```
