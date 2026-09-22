# OpenGate — MQTT Debug Commands

Documentation of `mosquitto_pub` and `mosquitto_sub` commands to emulate ESP32 and Android application without running them simultaneously.

## MQTT Architecture

- **Broker**: configured in `secrets.h`
- **Command topic**: `opengate/cmd` (QoS 1, persistent session)
- **ACK topic**: `opengate/ack` (QoS 0)
- **Command format**: `{"id":"<uuid>","command":"<command-name>"}`
- **ACK format**: `{"id":"<uuid>","result":"<result-code>","timestamp":"<iso-timestamp>"}`

## Supported Commands

| Command | Description | Result Codes |
|---------|-------------|-----------------|
| `OPEN` | Pulse relay + oscillate servo | `OK`, `DUPLICATE`, `RECOVERED`, `NVS_ERROR`, `FAILED` |

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
