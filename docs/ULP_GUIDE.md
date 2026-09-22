# ESP32 ULP Coprocessor — Technical Guide

## What is ULP?

The **Ultra Low Power (ULP)** is a 32-bit coprocessor present in ESP32 that:
- Executes code **even during main core deep sleep**
- Consumes **~10µA** (vs ~50µA of main core)
- Has limited access to GPIO, timers, ADC, and RTC memory
- Allows reducing power consumption up to 90%

## Hardware Specifications

| Aspect | Value |
|--------|-------|
| **CPU** | 32-bit RISC, 8 instructions/clock |
| **Clock** | ~100 kHz (during deep sleep) |
| **Registers** | 4 (R0-R3, 32-bit each) |
| **Memory** | 4 KB of RTC SRAM (persistent during sleep) |
| **GPIO** | Only RTCIO (from GPIO 0, 2, 4, 12, 13, 14, 15, 25-27, 32-39) |
| **Peripherals** | RTC Timer, ADC, RTC Temperature Sensor |
| **Consumption** | ~10µA @ 100 kHz |

## RTCIO (RTC Input/Output)

**Which GPIO support RTCIO?**
```
GPIO 0, 2, 4, 12, 13, 14, 15,     <- LOW-POWER-CAPABLE (internal)
GPIO 25, 26, 27,                  <- GPIO with RTC power domain
GPIO 32-39                        <- SAPPHIRE (RTC-only GPIO)
```

**Which GPIO NO:**
```
GPIO 1, 3, 5-11, 16-24, 28-31    <- Require VDDA power domain (unavailable in deep sleep)
```

**For OpenGate:** GPIO 15 (RTCIO 13) is ideal for ULP because it's supported and free.

## Use Case 1: Door Sensor (wakeup on change)

**Problem:** The magnetic sensor changes state, but we want the main core to wake up only then, not every 2 minutes.

**ULP Solution:**
```
┌─────────────────────────────────────────────┐
│ MAIN CORE (sleeping, ~1µA)                  │
│ - Deep sleep                                │
│ - Waits for interrupt from ULP              │
└─────────────────────────────────────────────┘
         ↑ wake up
         │
    ┌────┴────────────────────────────────────┐
    │ ULP (active, ~10µA)                     │
    │ - Monitor GPIO (magnetic sensor)        │
    │ - If state changes → wake main          │
    │ - Save timestamp in RTC memory          │
    └─────────────────────────────────────────┘
```

**Advantage:** Saves ~40µA while sensor doesn't change.

## Use Case 2: Physical Button (low power)

**Problem:** A button connected to GPIO, want to detect it even during deep sleep.

**ULP Solution:**
```
GPIO → ULP monitors → Loops until pressed → Wakes main core
       (~10µA wait)
```

**Debounce in ULP:** ULP can sample button multiple times to avoid false positives.

## Use Case 3: Counter/Timer (accurate)

**Problem:** Count seconds at low power without waking main core.

**ULP Solution:**
```c
// ULP pseudocode
counter = 0
while (true) {
    counter++
    delay(1 second)
    if (counter == 120) {  // 2 minutes
        wake_main_core()
    }
}
```

**Advantages:**
- No interrupt, no wake-up (until timer expires)
- Accuracy ±1ms
- Consumption: ~10µA

## How to Use ULP in OpenGate

### Option A: Door Sensor

```c
// ULP monitors magnetic sensor (GPIO 35 → RTCIO 9)
// If changes, wake main core
// Main core publishes state via MQTT and returns to sleep

void setup() {
    // Configure ULP to monitor GPIO 35
    rtcio_ll_set_direction(RTCIO_GPIO35_CHANNEL, RTC_GPIO_MODE_INPUT_ONLY);
    
    // Load ULP program that monitors GPIO
    ulp_load_binary(...);
    ulp_run(...);
    
    // Main core goes into deep sleep
    esp_deep_sleep_enable_ulp_wakeup();
    esp_deep_sleep(1000000); // 1 second (safety timeout)
}
```

### Option B: Physical Button

```c
// ULP monitors physical button (GPIO 15 → RTCIO 13)
// If pressed, wake main core

void setup() {
    rtcio_ll_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_INPUT_ONLY);
    
    // Load ULP program to detect state change
    ulp_load_binary(...);
    ulp_run(...);
    
    esp_deep_sleep_enable_ulp_wakeup();
    esp_deep_sleep(UINT64_MAX); // Sleep until pressed
}

void loop() {
    // Woken up: read command from GPIO or RTC memory
    // Publish via MQTT
    // Return to sleep
}
```

## ULP Limitations

| Limitation | Impact | Workaround |
|-----------|--------|-----------|
| **Only 4 registers** | Very simple programs | Use RTC memory for variables |
| **No floating point** | Integers only | Fixed-point or defer to main |
| **Limited GPIO access** | RTCIO only | Use GPIO with RTC capability |
| **No complex peripherals** | No I2C/SPI during sleep | Defer to main core |
| **Assembly language** | Hard for beginners | C wrappers exist for ESP-IDF |
| **Difficult debugging** | No Serial during sleep | Use RTC memory for logging |
| **Slow clock (~100kHz)** | Very long cycles | Acceptable for simple tasks |

## Performance: ULP vs Main Core

| Operation | Main Core | ULP | Savings |
|-----------|-----------|-----|---------|
| Monitor GPIO | 50µA | 10µA | **80%** |
| Simple timer | 50µA | 10µA | **80%** |
| Counter | 50µA | 10µA | **80%** |
| MQTT/WiFi | 80mA | Not supported | N/A |
| Deep sleep (idle) | 50µA | 10µA | **80%** |

## Integration with OpenGate

**Ideal scenario:** Using ULP to monitor low-power events:

```
NORMAL STATE:
├─ Deep sleep (main core)  ~1µA
├─ ULP monitoring          ~10µA
├─ WiFi disabled
├─ MQTT disabled
└─ Total consumption: ~11µA

EVENT (e.g., door sensor opens):
├─ ULP detects change
├─ Wakes main core
├─ Main core connects WiFi
├─ Publishes via MQTT
├─ Receives commands
├─ Returns to sleep
└─ Consumption during event: ~80mA for 5 seconds
```

**Battery savings:**
- Without ULP: 50µA * 24h = 1.2 Ah/day
- With ULP: 11µA * 24h = 0.26 Ah/day
- **Savings: ~80%**

## Resources

- [ESP-IDF ULP Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ulp.html)
- [ULP Instruction Set Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ulp_instruction_set.html)
- [Community Resource: PlatformIO + ULP](https://github.com/espressif/esp-idf/tree/master/examples/system/ulp)

## Next Steps for OpenGate

Choose which feature you want to implement:

1. **Door Sensor** (detect state change)
   - Difficulty: Medium
   - Benefit: High (no polling every 2 minutes)
   - Latency: <100ms

2. **Physical Unlock Button**
   - Difficulty: Medium
   - Benefit: High (offline unlock)
   - Latency: <100ms

3. **Accurate Low-Power Timer**
   - Difficulty: Low
   - Benefit: Medium (little savings vs main core)
   - Latency: ±1ms

Let me know which direction you want to take!
