# Build and Upload ULP with PlatformIO

## TL;DR — Quick Start

```bash
# 1. Create ULP folder
mkdir -p esp32/opengate/src/ulp

# 2. Write ULP program (Assembly)
# File: esp32/opengate/src/ulp/ulp_blink.S

# 3. Write C wrapper
# File: esp32/opengate/src/ulp_blink.c

# 4. Compile and upload
cd esp32/opengate
platformio run --target upload
```

---

## Step 1: Project Structure

Add this folder to your project:

```
esp32/opengate/
├── platformio.ini
├── src/
│   ├── opengate.ino              # Main sketch
│   ├── ulp_blink.c               # C wrapper for ULP
│   └── ulp/
│       └── ulp_blink.S           # ULP program (Assembly)
└── lib/
```

---

## Step 2: Write ULP Program (Assembly)

**File: `src/ulp/ulp_blink.S`**

```asm
/*
 * ULP Program: Blink LED
 * Turn on/off GPIO every 500ms
 * Executable during main core deep sleep
 */

.global entry
entry:
    # Load counter from RTC memory (address 0)
    # R0 will contain cycle count
    ld r0, r0, 0

    # Increment counter
    addi r0, r0, 1

    # If R0 < 25, jump to end_loop
    # 25 cycles * ~20ms = 500ms
    blti r0, 25, end_loop

    # If we reached 25 cycles, reset to 0
    movi r0, 0

    # TURN ON LED (GPIO 15 / RTCIO 13)
    gpios_wr_reg(RTC_GPIO_OUT_REG, (1 << 29), (1 << 29))
    
    # Wait ~250ms
    wait 250

    # TURN OFF LED
    gpios_wr_reg(RTC_GPIO_OUT_REG, (1 << 29), 0)
    
    # Wait ~250ms
    wait 250

end_loop:
    # Save counter to RTC memory
    st r0, r0, 0

    # Terminate ULP program
    # halt indicates ULP is done
    halt
```

**Or, SIMPLIFIED version (recommended for beginners):**

```asm
# File: src/ulp/ulp_blink.S
.global entry
entry:
    # Turn on LED
    movi r2, 1
    wr_reg RTC_GPIO_OUT_REG, 29, 1, 1
    
    # Delay cycles (~500ms at 100kHz)
    wait 50000
    
    # Turn off LED
    movi r2, 0
    wr_reg RTC_GPIO_OUT_REG, 29, 1, 0
    
    # Delay
    wait 50000
    
    # Loop
    jmp entry
    
    halt
```

---

## Step 3: C Wrapper to Load ULP

**File: `src/ulp_blink.c`**

```c
#include "ulp_blink.h"

// Compiler generates these symbols automatically
extern const uint32_t ulp_main_bin_start[];
extern const uint32_t ulp_main_bin_end;

// Load ULP program into memory and start it
void ulp_load_and_run() {
    esp_err_t err = esp_image_verify(
        ESP_IMAGE_BOOTLOADER,
        &bootloader_image_hdr,
        &verify_state
    );
    
    // Load ULP binary into RTC memory
    size_t ulp_size = (size_t)(&ulp_main_bin_end - ulp_main_bin_start);
    
    // Copy to RTC memory
    memcpy(
        (uint32_t*)RTC_SLOW_MEM,
        ulp_main_bin_start,
        ulp_size
    );
    
    // Configure and start ULP
    ulp_run((&ulp_main_bin_start - (uint32_t*)RTC_SLOW_MEM) / sizeof(uint32_t));
    
    Serial.println("[ULP] Program loaded and started");
}
```

**OR EVEN SIMPLER version (using ESP-IDF directly):**

```c
// File: src/ulp_blink.c
#include <esp32/ulp.h>
#include <driver/rtc_io.h>

// Assume ULP binary is provided as data array
extern const uint32_t ulp_blink_bin[] asm("_binary_ulp_blink_bin_start");
extern const uint32_t ulp_blink_bin_end asm("_binary_ulp_blink_bin_end");

void init_ulp_program() {
    // Load program
    esp_err_t err = ulp_load_binary(
        0,                                          // load_addr
        ulp_blink_bin,                              // program_binary
        (ulp_blink_bin_end - ulp_blink_bin) / 4     // program_size_words
    );
    
    if (err != ESP_OK) {
        Serial.printf("[ULP] Load error: %d\n", err);
        return;
    }
    
    // Start ULP program
    ulp_run(0);
    
    Serial.println("[ULP] Program started successfully");
}
```

---

## Step 4: Modify platformio.ini

**File: `platformio.ini`**

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_speed = 921600

# Enable ULP support
board_build.embed_files = src/ulp/ulp_blink.bin

# Include path for ESP-IDF
build_flags = 
    -I${PROJECT_DIR}/src
    -D ULPBLINK_USE_CUSTOM_IMAGE

lib_deps =
    knolleary/PubSubClient
    witnessmenow/UniversalTelegramBot
    madhephaestus/ESP32Servo
```

**Or, if using advanced build system:**

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = esp-idf

# For ESP-IDF framework (more control but more complex)
extra_scripts = post:extra_script.py

build_flags = 
    -DCONFIG_ULP_COPROC_ENABLED
```

---

## Step 5: Build Script (optional but useful)

**File: `extra_script.py`**

```python
Import("env", "projenv")
import os
from pathlib import Path

# Compile ULP Assembly files to binaries
def build_ulp_asm(source, target, env):
    ulp_asm_path = str(source[0])
    ulp_obj_path = ulp_asm_path.replace('.S', '.o')
    ulp_bin_path = ulp_asm_path.replace('.S', '.bin')
    
    # Compile with ESP32 ULP ASM toolchain
    cmd = f"xtensa-esp32-elf-gcc -c -I{env['PIOENV'].get('SDKCONFIG')} {ulp_asm_path} -o {ulp_obj_path}"
    os.system(cmd)
    
    # Linker
    cmd = f"xtensa-esp32-elf-ld {ulp_obj_path} -o {ulp_bin_path}"
    os.system(cmd)

# Hook before compiling
env.AddPreAction("buildprog", build_ulp_asm)
```

---

## Step 6: Integrate into Main Sketch

**File: `src/opengate.ino`**

```cpp
#include "ulp_blink.c"  // Include ULP wrapper

void setup() {
    Serial.begin(115200);
    delay(100);
    
    Serial.println("=== OpenGate with ULP ===");
    
    // Load ULP program
    init_ulp_program();
    
    // Configure GPIO for RTC (RTCIO 13 = GPIO 15)
    rtcio_hal_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);
    rtcio_hal_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtcio_hal_output_enable(RTCIO_GPIO15_CHANNEL);
    
    Serial.println("[ULP] Setup completed");
    
    delay(2000);
    
    // Main core goes into deep sleep
    esp_deep_sleep_enable_ulp_wakeup();
    esp_deep_sleep(10 * 1000000);  // 10 second timeout
}

void loop() {
    // Never reached (deep sleep)
}
```

---

## Step 7: Build and Upload

### Option A: Compile only

```bash
cd esp32/opengate
platformio run
```

### Option B: Compile and upload

```bash
cd esp32/opengate
platformio run --target upload
```

### Option C: Monitor + Upload

```bash
cd esp32/opengate
platformio run --target upload --target monitor
```

---

## Build Verification

After build, you should see:

```
Processing esp32dev (platform: espressif32; board: esp32dev; framework: arduino)
...
Linking .pio/build/esp32dev/firmware.elf
Generating binary .pio/build/esp32dev/firmware.bin
[SUCCESS] ✓ Built project
```

If you see ULP errors:

```
error: undefined reference to 'ulp_blink_bin_start'
```

It means the ULP binary was not generated. Check:
1. The `.S` file exists in `src/ulp/`
2. Extension is **UPPERCASE** `.S` (not `.s`)
3. PlatformIO rebuilt (clean `.pio/build/`)

---

## Debugging: Reading ULP Output

ULP cannot write to Serial. To debug:

```c
// Save data in RTC memory
RTC_SLOW_MEM[0] = counter;   // Read in main core via RTC_SLOW_MEM[0]
RTC_SLOW_MEM[1] = last_gpio;

// In main core (after wake):
uint32_t ulp_counter = RTC_SLOW_MEM[0];
uint32_t ulp_gpio_state = RTC_SLOW_MEM[1];
Serial.printf("ULP Counter: %lu\n", ulp_counter);
```

---

## Table: Compilation Methods

| Method | Difficulty | Control | Recommended |
|--------|-----------|---------|-------------|
| Arduino Framework + precompiled binary | Low | Low | ✅ Beginner |
| Arduino Framework + build script | Medium | Medium | ✅ Production |
| ESP-IDF Framework + ULP | High | High | ❌ Too complex |

---

## Quick Reference: Common ULP Instructions

```asm
# Data movement
movi r0, 123           # r0 = 123
ld r0, r0, 4           # load from RTC[4] to r0
st r0, r0, 4           # save r0 to RTC[4]

# Arithmetic
addi r0, r0, 1         # r0 = r0 + 1
subi r0, r0, 1         # r0 = r0 - 1
andi r0, r0, 0xFF      # r0 = r0 AND 0xFF
ori r0, r0, 0x01       # r0 = r0 OR 0x01

# Jumps
jmp label              # jump to label
beq r0, r1, label      # if r0 == r1, jump
blti r0, 100, label    # if r0 < 100, jump

# IO and Delay
wait 1000              # wait 1000 cycles (~10ms)
halt                   # terminate and exit

# GPIO
movi r2, 1             # r2 = 1 (to turn on)
wr_reg RTC_GPIO_OUT, 29, 1, 1  # turn on GPIO 15
```

---

## Common Issues

### Error: `undefined reference to ulp_main_bin_start`

**Cause:** ULP file was not compiled.

**Solution:**
```bash
# Clean and rebuild
rm -rf .pio/build/
platformio run
```

### ESP32 crashes after ULP load

**Cause:** GPIO not configured as RTCIO.

**Solution:**
```c
// Add before init_ulp_program():
rtcio_hal_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);
rtcio_hal_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);
```

### LED does not blink

**Cause:** GPIO not enabled or ULP not running.

**Debug:**
```c
// Read ULP status
esp_err_t err = ulp_run(0);
Serial.printf("ULP status: %d\n", err);

// Turn on manually for testing
digitalWrite(LED_PIN, HIGH);
delay(500);
digitalWrite(LED_PIN, LOW);
```

---

## Next: Implement Door Sensor

Once the LED blinks, the next step is:

1. Monitor GPIO 35 (magnetic sensor) in ULP
2. If state changes, wake main core
3. Main core publishes via MQTT

Want me to help with this step?
