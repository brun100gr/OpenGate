# OpenGate

IoT gate opener: a button in an Android app (phone **and** Android Auto)
publishes an MQTT message to a cloud broker; an ESP32 subscribed to the same
topic closes a relay wired to the gate control board. The same press also
starts streaming the phone position to the broker for five minutes.

```
Phone / Android Auto ──(MQTT/TLS, mobile network)──► HiveMQ Cloud ──(MQTT/TLS, WiFi)──► ESP32 ──► relay ──► gate control board
```

## Repository Layout

- `android/` — Android app in Kotlin (phone UI + Android Auto, IOT category)
- `esp32/opengate/` — Arduino sketch for the ESP32

## Credentials

Credentials never reach the repository: the real configuration files are
gitignored, and only `.example` templates are committed.

| Real file (gitignored) | Committed template |
|---|---|
| `android/app/src/main/java/com/opengate/Config.kt` | `Config.kt.example` |
| `esp32/opengate/secrets.h` | `secrets.h.example` |

After cloning, copy each template next to itself, dropping the `.example`
suffix, and fill in your values. The project does not build until you do.

### Environment Switching (Test ↔ Production)

Both `Config.kt` (Android) and `secrets.h` (ESP32) support switching between
test and production brokers **without changing any code**:

**Android (`Config.kt`)**:
```kotlin
private const val IS_TEST = true  // Change to false for production
```

**ESP32 (`secrets.h`)**:
```c
#define ENVIRONMENT_TEST
// #define ENVIRONMENT_PROD
```

Both test and production credentials are baked into the files (which are
gitignored). Simply flip the flag, recompile, and the app or sketch connects
to the other broker. Useful for validating changes in a test environment
before deploying to production.

---

## 1. MQTT Broker (HiveMQ Cloud)

1. Sign up at <https://console.hivemq.cloud> and create a **Serverless
   (free)** cluster: it includes TLS and is reachable from the Internet, so
   it also works when you are in the car on the mobile network.
2. Note down the cluster **hostname** (e.g. `abc12345.s1.eu.hivemq.cloud`)
   and port **8883**.
3. In **Access Management** create two separate credentials (so they can be
   revoked independently):
   - `opengate-app` → used by the Android app (*publish* permission)
   - `opengate-esp32` → used by the ESP32 (*subscribe* permission)

   If the free plan does not allow granular permissions, that's fine: still
   use two distinct users with strong passwords.

   If you *did* restrict permissions per topic, remember to allow
   `opengate-app` to publish on `opengate/gps` as well, not just on
   `opengate/cmd` — otherwise the position stream is rejected silently.

## 2. Android App

You need [Android Studio](https://developer.android.com/studio) (it downloads
the proper SDK and JDK on its own).

1. Open Android Studio → **Open** → select the `android/` folder.
   On first import Gradle downloads the dependencies and, if the wrapper jar
   is missing, Android Studio offers to regenerate it: accept.
2. In `app/src/main/java/com/opengate/`, copy `Config.kt.example` to
   `Config.kt` and fill in the credentials for both your **test** and
   **production** HiveMQ brokers. At the top of `Config.kt`, choose your
   environment by setting `IS_TEST = true` (default) or `IS_TEST = false`
   for production.
3. Connect the phone via USB (with **USB debugging** enabled in developer
   options) and press **Run ▶**. The app appears on the phone: a single
   "Open gate" button.

### GPS Position Streaming

Pressing "Open gate" does two independent things: it publishes the `OPEN`
command, and it starts a five-minute tracking session that publishes the phone
position to `opengate/gps` once a second, with **QoS 0** — a lost fix is simply
replaced by the next one a second later. Pressing the button again during a
session restarts the five minutes without reconnecting.

The tracking runs in a foreground service, so it keeps going with the screen
off or the app in the background — which is the whole point, since the phone is
normally in a car mount. While it is active a silent, persistent notification
is shown; Android requires it and it doubles as the way to tell the stream is
running. The session ends on its own after five minutes and the notification
disappears.

The first press asks for the **location** permission (and, on Android 13+, for
the notification permission). Both are optional: if you refuse, the gate still
opens and only the position stream is skipped. On Android Auto no dialog is
ever shown — grant the permission once from the phone UI, otherwise the car
screen just reports that no position was sent.

The payload format is documented in [MQTT_DEBUG.md](MQTT_DEBUG.md), which also
shows how to watch the stream with `mosquitto_sub`.

### Enabling the App on Android Auto (without the Play Store)

Apps not coming from the Play Store must be explicitly enabled:

1. On the phone open **Android Auto** (on Android 10+ it lives under
   *Settings → Apps → Android Auto → Additional settings*).
2. Scroll down to **Version** and tap it **10 times** → developer mode is
   enabled.
3. From the ⋮ menu → **Developer settings** → enable **Unknown sources**.
4. Connect the phone to the car: the Android Auto launcher now shows
   **OpenGate** with the gate icon. Tapping it opens the grid with the
   "Open gate" button.

> To test without a car you can use Google's **Desktop Head Unit (DHU)**,
> which emulates the car screen on your PC:
> <https://developer.android.com/training/cars/testing/dhu>

## 3. ESP32

### Software

1. In the Arduino IDE install the ESP32 board support (*Boards Manager* →
   "esp32" by Espressif) and the **PubSubClient** library (Nick O'Leary).
2. In `esp32/opengate/`, copy `secrets.h.example` to `secrets.h` and fill it
   in: WiFi SSID/password, and credentials for both your **test** and
   **production** HiveMQ clusters. At the top of `secrets.h`, choose your
   environment by setting `#define ENVIRONMENT_TEST` (default) or
   `#define ENVIRONMENT_PROD`.
3. Upload the sketch and open the serial monitor at 115200 baud: it should
   print `WiFi OK`, `Connecting to MQTT... OK`, `Subscribed to opengate/cmd`.

### TLS Certificates

Root CA certificates live in `esp32/opengate/src/certificates.h`. They are
public CA material, not secrets, so that file is committed to the repository —
you normally do not need to touch it.

- `MQTT_ROOT_CA` — **ISRG Root X1**, the Let's Encrypt trust anchor used by
  HiveMQ Cloud (<https://letsencrypt.org/certificates/>). If this is ever
  replaced by a non-certificate placeholder, the sketch falls back to
  `setInsecure()`: traffic is still encrypted but the ESP32 does not verify it
  is talking to the real broker.
- `TELEGRAM_ROOT_CA` — **Go Daddy Root Certificate Authority - G2**, the trust
  anchor currently used by `api.telegram.org`. There is no insecure fallback
  here: a missing certificate just means notifications stop working.

Update a certificate here when the corresponding service rotates its trust
anchor.

### Wiring

With a common **relay module** (opto-isolated, 3.3V-drivable):

| Relay module | ESP32 |
|---|---|
| VCC | 5V (VIN) or 3V3 depending on the module |
| GND | GND |
| IN  | GPIO 26 |

The relay's dry contacts (**COM** and **NO**, normally open) go in parallel
with the "open button" / "start" input of the gate control board — the same
terminals where a wall button or a key switch would be wired. The 1 s pulse
(configurable via `PULSE_MS`) is equivalent to a button press.

⚠️ Work on the control board **with power disconnected** and check the
manual for which terminal pair is the button input: they are extra-low
voltage contacts, but wiring the wrong terminals can damage the board.

## 4. Android Auto UI

To test the Android Auto UI without a physical vehicle, use the **Desktop Head Unit (DHU)** — Google's official emulator that runs on your PC (Windows, macOS, or Linux) and simulates the car screen, communicating with your phone via USB or ADB.

### Setup

**Option A: Direct DHU (requires GLIBC 2.32+)**

1. **Install DHU** in Android Studio:
   - Open *Settings → Languages & Frameworks → Android SDK → SDK Tools*
   - Find and install *Android Auto Desktop Head Unit Emulator*
   - The executable lands in `[SDK_PATH]/extras/google/auto/`

2. **Prepare the Phone**:
   - Open *Android Auto settings* (search in phone settings or open the Android Auto app)
   - Scroll to the bottom and tap **Version** 10 times to unlock developer settings
   - Open the menu ⋮ and select **Start head unit server**
   - You should see a persistent notification: "Head unit server running"

3. **Connect and Launch**:
   - Connect the phone to your PC via USB
   - Open a terminal and set up ADB port forwarding:
     ```sh
     ~/Android/Sdk/platform-tools/adb forward tcp:5277 tcp:5277
     ```
   - Run the DHU from the SDK folder:
     ```sh
     # macOS/Linux
     ./desktop-head-unit
     ```

**Option B: DHU via Docker (recommended for Ubuntu 20.04 or older)**

If your system lacks GLIBC 2.32+:

1. **Create a Dockerfile** in the DHU directory (`[SDK_PATH]/extras/google/auto/`):
   ```dockerfile
   FROM ubuntu:22.04
   RUN apt-get update && apt-get install -y \
       libc++1 libc++abi1 libusb-1.0-0 \
       libsdl2-2.0-0 libsdl2-ttf-2.0-0 libportaudio2 libpng16-16 x11-apps
   WORKDIR /dhu
   ENTRYPOINT ["./desktop-head-unit"]
   ```

2. **Build the Image**:
   ```sh
   docker build -t android-dhu .
   ```

3. **Run via Docker** (enable X11 display):
   ```sh
   xhost +local:docker
   docker run -it --rm \
       -e DISPLAY=$DISPLAY \
       -v /tmp/.X11-unix:/tmp/.X11-unix \
       -v $(pwd):/dhu \
       --net=host --privileged \
       android-dhu
   ```

The phone and DHU should auto-connect; you'll see the OpenGate grid with the "Open gate" button on the emulated car screen.

## 5. End-to-End Test

1. ESP32 powered and connected (serial monitor open).
2. From the phone app press **Open gate** → the app shows "Command sent ✓",
   the serial monitor prints "Valid command: opening the gate", and the
   relay clicks for one second.
3. Subscribe to `opengate/gps` (see [MQTT_DEBUG.md](MQTT_DEBUG.md)) and press
   the button again: one position per second arrives for five minutes, then
   the stream stops by itself.
4. Repeat from Android Auto (or from the DHU).

For debugging you can also publish manually from a PC with the Mosquitto
clients:

```sh
mosquitto_pub -h YOUR_CLUSTER.s1.eu.hivemq.cloud -p 8883 \
  -u opengate-app -P 'THE_PASSWORD' --capath /etc/ssl/certs \
  -t opengate/cmd -m open
```

## 6. Security Notes

This command opens your home, so:

- **Already included**: TLS on both legs, authentication with separate
  credentials for the app and the ESP32, messages never `retained` (an
  "open" command must not linger on the broker), QoS 1 on the command
  (QoS 0 on the position stream, where a lost message costs nothing).
- **To do manually**: paste the root CA into the sketch (see above) to
  prevent MITM on the ESP32 side.
- **Known limitation**: anyone who obtains the app credentials can open the
  gate; the credentials are compiled into the APK, so do not share the APK.
  Possible evolution: signed payload with a timestamp (HMAC) to prevent
  replay even if the broker is compromised.
- **Privacy**: `opengate/gps` carries your real position. The same credentials
  that open the gate can subscribe to it, so losing the APK also means leaking
  five minutes of your movements around home each time the gate is opened.
