# MQTT OpenGate – Android ↔ ESP32 Simulator

This project provides two Python scripts that simulate the MQTT communication between an Android application and an ESP32-based gate opener.

The purpose of the project is to test how MQTT commands behave when the ESP32 is periodically offline, for example when the ESP32 is battery-powered and spends most of its time in deep sleep.

The simulators use the command-line MQTT clients `mosquitto_pub` and `mosquitto_sub` and connect to an MQTT broker such as HiveMQ Cloud.

## Architecture

The simulated system consists of three components:

```text
┌──────────────┐
│ Android App  │
│  Simulator   │
└──────┬───────┘
       │
       │ MQTT QoS 1
       │ retain=false
       ▼
┌──────────────────┐
│   MQTT Broker    │
│    HiveMQ Cloud  │
└────────┬─────────┘
         │
         │ Queued message
         │ while ESP32 is offline
         ▼
┌──────────────────┐
│      ESP32       │
│    Simulator     │
│                  │
│ Wake → MQTT →    │
│ command → GPIO   │
│ → ACK → Sleep    │
└──────────────────┘
```

### Android simulator

`android_simulator.py` simulates the Android application.

It:

1. Generates a unique UUID for every command.
2. Creates a JSON MQTT message.
3. Publishes the message with QoS 1.
4. Does not use MQTT retained messages.

Example payload:

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "command": "OPEN",
  "timestamp": "2026-09-02T12:00:00+00:00"
}
```

### ESP32 simulator

`esp32_simulator.py` simulates an ESP32 that periodically wakes up and connects to MQTT.

It uses:

- MQTT 3.1.1
- QoS 1
- a fixed MQTT client ID
- a persistent MQTT session (`clean-session=false`)
- a local JSON file to simulate ESP32 NVS storage
- an ACK topic

When a command is received, the simulator checks whether its ID has already been processed.

If it is a new command:

```text
MQTT command
     ↓
OPEN command
     ↓
Simulated GPIO activation
     ↓
Save command ID
     ↓
Publish ACK
     ↓
Deep sleep
```

If the command ID has already been processed, the GPIO is not activated again.

## MQTT Topics

The default topics are:

| Purpose | Topic |
|---|---|
| Commands | `opengate/cmd` |
| Acknowledgements | `opengate/ack` |

These values can be changed through the `.env` file.

## Why QoS 1 and a Persistent Session?

The ESP32 is assumed to be offline most of the time because it enters deep sleep.

A retained MQTT message is **not** used for the `OPEN` command because `OPEN` is an event, not a state.

A retained message would remain on the broker and could be delivered again every time the ESP32 subscribes.

Instead, this project uses:

- `retain=false`
- QoS 1
- a persistent MQTT session
- a fixed client ID

With MQTT 3.1.1, the persistent session is created by using:

```text
clean-session=false
```

The `mosquitto_sub` option used by the ESP32 simulator is:

```text
-c
```

The broker can therefore queue QoS 1 messages for the ESP32 while it is offline, as long as the persistent session still exists.

The client ID is important: it must remain the same between wake-up cycles.

## Command Deduplication

MQTT QoS 1 provides **at-least-once delivery**.

This means that a message can potentially be delivered more than once.

For a gate opener this is important because executing the same command twice could cause an unwanted second GPIO activation.

Every command therefore contains a unique UUID:

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "command": "OPEN"
}
```

The ESP32 simulator stores the IDs of recently processed commands in:

```text
esp32_nvs_sim.json
```

This file simulates persistent ESP32 NVS storage.

The simulator currently keeps the last 20 processed command IDs.

For example:

```json
{
  "processed_command_ids": [
    "550e8400-e29b-41d4-a716-446655440000"
  ],
  "last_command_id": "550e8400-e29b-41d4-a716-446655440000"
}
```

If the same command is received again, the simulator reports:

```text
DUPLICATE
```

and does not activate the simulated GPIO.

## Acknowledgements

After processing a command, the ESP32 simulator publishes an ACK on:

```text
opengate/ack
```

Example:

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "result": "OK",
  "timestamp": "2026-09-02T12:01:00+00:00"
}
```

Possible results are:

- `OK` – command was executed.
- `DUPLICATE` – command was already processed.
- `UNKNOWN_COMMAND` – command is not supported.

## Configuration

MQTT credentials and other configuration parameters are stored in a local `.env` file.

### `.env.example`

The repository should contain:

```text
.env.example
```

This file is a template containing example values and **must not contain real credentials**.

Example:

```dotenv
MQTT_HOST=xxxxxx.s1.eu.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=my_username
MQTT_PASSWORD=my_password

MQTT_CMD_TOPIC=opengate/cmd
MQTT_ACK_TOPIC=opengate/ack

MQTT_CA_PATH=/etc/ssl/certs/
MQTT_CLIENT_ID=ESP32_GATE_SIM_01
MQTT_STATE_FILE=./esp32_nvs_sim.json
```

### Local `.env`

Create your local configuration by copying the template:

```bash
cp .env.example .env
```

Then edit it:

```bash
nano .env
```

and replace the example credentials with your actual HiveMQ Cloud credentials.

For example:

```dotenv
MQTT_HOST=your-cluster.s1.eu.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=your_real_username
MQTT_PASSWORD=your_real_password

MQTT_CMD_TOPIC=opengate/cmd
MQTT_ACK_TOPIC=opengate/ack

MQTT_CA_PATH=/etc/ssl/certs/
MQTT_CLIENT_ID=ESP32_GATE_SIM_01
MQTT_STATE_FILE=./esp32_nvs_sim.json
```

**Never commit `.env` to Git.**

The `.gitignore` file should contain at least:

```gitignore
.env
.venv/
venv/
__pycache__/
*.py[cod]
esp32_nvs_sim.json
```

The Python scripts load the `.env` file using `python-dotenv`.

## Requirements

### System requirements

On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install python3 python3-venv mosquitto-clients
```

The `mosquitto-clients` package provides:

- `mosquitto_pub`
- `mosquitto_sub`

Check that they are available:

```bash
mosquitto_pub --help
mosquitto_sub --help
```

### Python dependencies

The Python scripts use:

```text
python-dotenv
```

The `requirements.txt` file should therefore contain:

```text
python-dotenv
```

`paho-mqtt` is **not required by the current implementation**, because the scripts communicate with the broker through the `mosquitto_pub` and `mosquitto_sub` command-line tools.

## Installation

Clone the repository:

```bash
git clone <repository-url>
cd OpenGate/local
```

Create a Python virtual environment:

```bash
python3 -m venv venv
```

Activate it:

```bash
source venv/bin/activate
```

Install the Python dependencies:

```bash
python -m pip install -r requirements.txt
```

Using:

```bash
python -m pip
```

instead of simply:

```bash
pip
```

helps ensure that the packages are installed into the currently active virtual environment.

You can verify the installation with:

```bash
python -c "import dotenv; print('python-dotenv OK')"
```

Create the local configuration:

```bash
cp .env.example .env
```

Edit the credentials:

```bash
nano .env
```

## Testing the Persistent MQTT Session

The following test verifies that a command published while the ESP32 is offline can be received when it wakes up again.

### 1. Create the ESP32 persistent session

Start the ESP32 simulator and leave it connected:

```bash
./esp32_simulator.py --cycles 1 --awake-timeout 300
```

Wait until the MQTT connection and subscription have been established.

Then terminate the simulator with:

```text
Ctrl+C
```

The important point is that the MQTT client must have established its persistent session before being disconnected.

The client ID must remain unchanged.

### 2. Publish a command while the ESP32 is offline

Run:

```bash
./android_simulator.py
```

The simulator generates a new UUID and publishes an `OPEN` command with QoS 1.

At this point the ESP32 simulator is offline.

The broker should keep the message in the persistent session.

### 3. Wake up the ESP32

Run:

```bash
./esp32_simulator.py --cycles 1 --awake-timeout 15
```

The ESP32 simulator should receive the queued command and simulate the gate opening.

You should see something similar to:

```text
[ESP32] *** GPIO -> OPEN ***
[ESP32] *** Gate activated ***
```

The command ID is then stored in:

```text
esp32_nvs_sim.json
```

and an ACK is published on:

```text
opengate/ack
```

### 4. Wake the ESP32 again

Run:

```bash
./esp32_simulator.py --cycles 1 --awake-timeout 15
```

The same command should not cause another gate activation.

The command has already been processed and its ID is stored locally.

## Simulating Periodic Deep Sleep

The simulator can reproduce a simplified version of the real ESP32 behavior:

```text
Wake
 ↓
Connect to MQTT
 ↓
Wait for command
 ↓
Execute command
 ↓
Publish ACK
 ↓
Deep sleep
 ↓
Wake again
 ↓
...
```

For example:

```bash
./esp32_simulator.py --cycles 20 --sleep 60 --awake-timeout 10
```

This simulates 20 wake/sleep cycles with:

- 10 seconds of MQTT listening time
- 60 seconds of simulated deep sleep between cycles

While the ESP32 simulator is sleeping, publish a command using:

```bash
./android_simulator.py
```

The broker should queue the command until the ESP32 reconnects.

## Monitoring the MQTT Traffic

The MQTT traffic can also be monitored manually with `mosquitto_sub`.

For example, to monitor the command topic:

```bash
mosquitto_sub \
  -h "$MQTT_HOST" \
  -p "$MQTT_PORT" \
  -u "$MQTT_USERNAME" \
  -P "$MQTT_PASSWORD" \
  --capath "$MQTT_CA_PATH" \
  -V mqttv311 \
  -q 1 \
  -t "opengate/cmd" \
  -v \
  -d
```

Alternatively, load the variables from `.env` into the shell before running the command, or use the Python simulators which load `.env` automatically.

## Command-Line Options

### Android simulator

Run:

```bash
./android_simulator.py --help
```

Currently supported command:

```bash
./android_simulator.py --command OPEN
```

The default command is:

```text
OPEN
```

### ESP32 simulator

Run:

```bash
./esp32_simulator.py --help
```

Available options include:

```text
--cycles
```

Number of wake/sleep cycles to simulate.

Default:

```text
1
```

```text
--sleep
```

Seconds of simulated deep sleep between wake-ups.

Default:

```text
60
```

```text
--awake-timeout
```

Maximum time in seconds during which the simulator waits for an MQTT command.

Default:

```text
15
```

```text
--state-file
```

Path to the JSON file used to simulate ESP32 NVS storage.

Default:

```text
./esp32_nvs_sim.json
```

Example:

```bash
./esp32_simulator.py \
    --cycles 20 \
    --sleep 60 \
    --awake-timeout 10 \
    --state-file ./test_state.json
```

## Project Structure

A typical repository layout is:

```text
OpenGate/
└── local/
    ├── android_simulator.py
    ├── esp32_simulator.py
    ├── README.md
    ├── requirements.txt
    ├── .env.example
    ├── .gitignore
    └── venv/
```

The following files should **not** be committed:

```text
.env
venv/
.venv/
__pycache__/
esp32_nvs_sim.json
```

The `.env.example` file **should** be committed because it documents the required configuration without exposing real credentials.

## Security

Never store MQTT credentials directly in the Python source code.

Do not commit:

```text
MQTT_USERNAME=real_username
MQTT_PASSWORD=real_password
```

to Git.

Use:

```text
.env
```

for local credentials and:

```text
.env.example
```

for safe example configuration.

Before pushing the project to GitHub, verify:

```bash
git status
```

and make sure `.env` is not listed as a file to be committed.

If a real MQTT password has already been committed to Git history, simply deleting the file is not sufficient. The credential should be changed/revoked in the MQTT broker and the Git history should be cleaned if necessary.

## Important MQTT Semantics

This project demonstrates **at-least-once delivery**, not exactly-once execution.

MQTT QoS 1 guarantees that the broker attempts to deliver the message at least once, but duplicates are possible.

The combination of:

```text
QoS 1
+
persistent session
+
unique command ID
+
persistent command-ID storage
```

is used here to make the gate command robust against normal MQTT reconnections and duplicate deliveries.

There is still an important failure case to consider in a real ESP32 implementation:

```text
Receive command
      ↓
Activate GPIO
      ↓
ESP32 loses power
      ↓
Command ID was not persisted / ACK was not sent
      ↓
Command may be delivered again
```

The simulator is intended to demonstrate the MQTT architecture and command deduplication strategy. The final ESP32 firmware should carefully define the ordering between hardware activation, persistent storage, and ACK transmission.

## Current Scope

This project is a test environment for the MQTT communication architecture.

It does **not** yet represent the final ESP32 firmware.

In particular, the ESP32 simulator does not implement:

- actual ESP32 deep sleep
- Wi-Fi power management
- GPIO hardware
- the physical gate controller
- RTC handling
- battery management
- real ESP32 NVS
- automatic MQTT reconnection inside a single long-running MQTT client

Those features will be implemented and tested later on the actual ESP32 hardware.