# ULP with PlatformIO — Simple Working Method

External Assembly approach (`xtensa-esp32-elf-as`) has syntax issues. We use the **raw binary method in C** which is simpler and more reliable.

---

## ⚡ TL;DR — 3 minutes to test

```bash
# 1. Copy test sketch
cp examples/opengate_ulp_test.ino esp32/opengate/src/opengate.ino

# 2. Upload to ESP32
cd esp32/opengate
platformio run --target upload --target monitor

# 3. Connect LED to GPIO 15
# Result: LED blinks, main core in deep sleep (~10µA)
```

---

## 📝 How It Works

### ULP Program is an Array of uint32_t

Instead of writing Assembly that must be compiled, we write instructions directly as numbers (raw encoding).

```c
// This is the ULP program:
static const uint32_t ulp_program[] = {
    MOVI(0, 1),              // R0 = 1
    WR_REG(0x54, 29, 1, 1),  // Turn on GPIO 15
    WAIT(50000),             // Wait ~500ms
    MOVI(0, 0),              // R0 = 0
    WR_REG(0x54, 29, 1, 0),  // Turn off GPIO 15
    WAIT(50000),             // Wait ~500ms
    JMP(0),                  // Loop
    HALT(),                  // End
};

// Macros (MOVI, WR_REG, etc) convert instructions to uint32_t
```

### No External Compilation

- ✅ No `xtensa-esp32-elf-as`
- ✅ No Assembly `.S` files
- ✅ No Python build script
- ✅ All pure C
- ✅ PlatformIO compiles directly

---

## 🚀 Step by Step

### Step 1: Sketch File

Use `examples/opengate_ulp_test.ino` — contains:
- ULP program definition
- GPIO 15 setup
- Load and run ULP
- Deep sleep

```c
void setup() {
    // Configure GPIO 15
    rtcio_ll_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);
    rtcio_ll_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtcio_ll_output_enable(RTCIO_GPIO15_CHANNEL);

    // Load program
    ulp_load_binary(0, ulp_program, sizeof(ulp_program)/4);

    // Start
    ulp_run(0);

    // Sleep
    esp_deep_sleep(10 * 1000000);
}
```

### Step 2: Upload

```bash
cd esp32/opengate
platformio run --target upload --target monitor
```

### Step 3: Expected Output

```
================== OpenGate ULP Blink ==================

[GPIO] Configuring GPIO 15 as RTC output...
[GPIO] ✓ GPIO 15 configured

[ULP] Loading program...
[ULP] ✓ Program loaded

[ULP] Starting program...
[ULP] ✓ Program running

Behavior:
  - LED blinks every 500ms
  - Main core sleeps for 10 seconds
  - Consumption: ~10µA

[SLEEP] Entering deep sleep for 10 seconds...
```

---

## 🔌 Hardware Setup

| Component | GPIO |
|-----------|------|
| LED + (through 330Ω resistor) | GPIO 15 |
| LED - | GND |

```
        ┌──[330Ω]──┐
        │           │
      +5V           │
        │           ▼
        └──────── LED ──────── GPIO 15
                    │
                   GND
```

---

## 🔧 Modifiable Parameters

### Change LED Timing

In `examples/opengate_ulp_test.ino`, modify the `WAIT`:

```c
WAIT(50000),   // Change this to modify duration
```

| Value | Duration |
|-------|----------|
| 10000 | ~100ms |
| 50000 | ~500ms |
| 100000 | ~1 second |

### Change GPIO

To use different GPIO:

```c
// Find bit corresponding to GPIO
// GPIO 15 = bit 29
// GPIO 14 = bit 28
// GPIO 13 = bit 27
// ...

WR_REG(0x54, 29, 1, 1),  // Change this 29
```

GPIO → Bit Mapping:
```
GPIO 15 → Bit 29
GPIO 14 → Bit 28
GPIO 13 → Bit 27
GPIO 12 → Bit 26
GPIO 4  → Bit 25
...
```

---

## ⚙️ Available ULP Macros

```c
// Load value into register
MOVI(Rd, Imm)

// Write to RTC register (GPIO output, config, etc)
WR_REG(Addr, Offset, Len, Data)

// Wait N cycles
WAIT(Cycles)

// Jump to instruction
JMP(Offset)

// Subtract
SUB(Rd, Rs, Rt)

// Subtract immediate
SUBI(Rd, Rs, Imm)

// Branch if greater or equal
BGEI(Rs, Imm, Offset)

// Terminate ULP
HALT()
```

---

## 📊 Verify Consumption

After 10 seconds of sleep with ULP active:
- Multimeter on GPIO 15: 0V (LED off) or 3.3V (LED on)
- Consumption: ~10µA (vs ~50µA without ULP)

To measure:
1. Connect ammeter in series with battery
2. Main core in sleep: read consumption
3. Should be ~10µA (with some mA spikes when LED changes)

---

## 🐛 Troubleshooting

### LED doesn't turn on

**Cause 1:** GPIO not configured correctly

**Check:**
```c
// Verify in setup() before loading ULP:
rtcio_ll_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);
rtcio_ll_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);
rtcio_ll_output_enable(RTCIO_GPIO15_CHANNEL);
```

**Cause 2:** ULP not loaded

**Check:**
```c
esp_err_t err = ulp_load_binary(0, ulp_program, sizeof(ulp_program)/4);
if (err != ESP_OK) {
    Serial.printf("ULP error: %d\n", err);
}
```

### Sketch compiles but crashes on upload

**Cause:** Missing include path

**Solution:** Ensure platformio.ini has:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
```

### LED turns on but doesn't turn off

**Cause:** ULP program not looping correctly

**Debug:** Add a loop counter:
```c
// In ULP program, before HALT:
JMP(0),  // ← Make sure this is here
```

---

## 💾 Include in Main OpenGate

To add ULP to the real OpenGate project:

**File: `src/opengate.ino`**

```cpp
#include <esp32/ulp.h>
#include <driver/rtc_io.h>

// Copy macros and ULP program from opengate_ulp_test.ino
static const uint32_t ulp_program[] = {
    MOVI(0, 1),
    WR_REG(0x54, 29, 1, 1),
    // ... rest of program
};

void setup() {
    // ... existing setup ...

    // Load ULP (optional, can be enabled/disabled)
    if (ENABLE_ULP) {  // Define this constant
        ulp_load_binary(0, ulp_program, sizeof(ulp_program)/4);
        ulp_run(0);
    }

    // ... rest of setup ...
}
```

---

## ✅ Final Checklist

- [ ] Sketch `opengate_ulp_test.ino` uploaded
- [ ] LED connected to GPIO 15
- [ ] Compilation completed (no errors)
- [ ] Upload completed
- [ ] Monitor shows correct output
- [ ] LED blinks for 10 seconds
- [ ] Main core sleeps after blinking

If everything ✅, ULP works!

---

## 🎯 Next Steps

1. **Integrate in OpenGate**: Add ULP code to opengate.ino
2. **Monitor sensor**: Use ULP to monitor GPIO (door sensor)
3. **Reduce consumption**: Main core wakes only on events, not every 2 minutes

Questions? Review `ULP_GUIDE.md` for technical details.
