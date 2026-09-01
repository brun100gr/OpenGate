# OpenGate

IoT gate opener: a button in an Android app (phone **and** Android Auto)
publishes an MQTT message to a cloud broker; an ESP32 subscribed to the same
topic closes a relay wired to the gate control board.

```
Phone / Android Auto ──(MQTT/TLS, mobile network)──► HiveMQ Cloud ──(MQTT/TLS, WiFi)──► ESP32 ──► relay ──► gate control board
```

Repository layout:

- `android/` — Android app in Kotlin (phone UI + Android Auto, IOT category)
- `esp32/opengate/` — Arduino sketch for the ESP32

## Secrets

Credentials never reach the repository: the real configuration files are
gitignored, and only `.example` templates are committed.

| Real file (gitignored) | Committed template |
|---|---|
| `android/app/src/main/java/com/opengate/Config.kt` | `Config.kt.example` |
| `esp32/opengate/secrets.h` | `secrets.h.example` |

After cloning, copy each template next to itself, dropping the `.example`
suffix, and fill in your values. The project does not build until you do.

---

## 1. MQTT broker (HiveMQ Cloud)

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

## 2. Android app

You need [Android Studio](https://developer.android.com/studio) (it downloads
the proper SDK and JDK on its own).

1. Open Android Studio → **Open** → select the `android/` folder.
   On first import Gradle downloads the dependencies and, if the wrapper jar
   is missing, Android Studio offers to regenerate it: accept.
2. In `app/src/main/java/com/opengate/`, copy `Config.kt.example` to
   `Config.kt` and fill in the cluster hostname and the `opengate-app`
   credentials.
3. Connect the phone via USB (with **USB debugging** enabled in developer
   options) and press **Run ▶**. The app appears on the phone: a single
   "Open gate" button.

### Enabling the app on Android Auto (without the Play Store)

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
   in: WiFi SSID/password, HiveMQ hostname, `opengate-esp32` credentials.
3. Upload the sketch and open the serial monitor at 115200 baud: it should
   print `WiFi OK`, `Connecting to MQTT... OK`, `Subscribed to opengate/cmd`.

### TLS certificate

HiveMQ Cloud uses Let's Encrypt certificates. For full server verification,
download the **ISRG Root X1** root CA (PEM format) from
<https://letsencrypt.org/certificates/> and paste the
`-----BEGIN CERTIFICATE----- … -----END CERTIFICATE-----` block into the
`ROOT_CA` constant in `secrets.h`. If you leave the placeholder, the sketch
uses `setInsecure()`: traffic is still encrypted but the ESP32 does not
verify it is talking to the real broker — acceptable for early testing, to
be fixed before real use.

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

## 4. End-to-end test

1. ESP32 powered and connected (serial monitor open).
2. From the phone app press **Open gate** → the app shows "Command sent ✓",
   the serial monitor prints "Valid command: opening the gate", and the
   relay clicks for one second.
3. Repeat from Android Auto (or from the DHU).

For debugging you can also publish manually from a PC with the Mosquitto
clients:

```sh
mosquitto_pub -h YOUR_CLUSTER.s1.eu.hivemq.cloud -p 8883 \
  -u opengate-app -P 'THE_PASSWORD' --capath /etc/ssl/certs \
  -t opengate/cmd -m open
```

## 5. Security notes

This command opens your home, so:

- **Already included**: TLS on both legs, authentication with separate
  credentials for the app and the ESP32, messages never `retained` (an
  "open" command must not linger on the broker), QoS 1.
- **To do manually**: paste the root CA into the sketch (see above) to
  prevent MITM on the ESP32 side.
- **Known limitation**: anyone who obtains the app credentials can open the
  gate; the credentials are compiled into the APK, so do not share the APK.
  Possible evolution: signed payload with a timestamp (HMAC) to prevent
  replay even if the broker is compromised.
