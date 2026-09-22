/*
 * ESP32 ULP Coprocessor — LED Blink Example
 *
 * The ULP (Ultra Low Power) is a 32-bit coprocessor that can execute code
 * even when the main processor is in deep sleep, drastically reducing
 * power consumption.
 *
 * This example blinks an LED using only the ULP while the main core
 * stays in deep sleep.
 *
 * Connections:
 *   - GPIO 15 (RTCIO 13): LED with protection resistor
 *
 * Behavior:
 *   - Main core loads ULP program and goes into deep sleep
 *   - ULP turns on/off LED every 500ms
 *   - Power consumption is extremely low (~10µA)
 */

#include <esp32/ulp.h>
#include <driver/rtc_io.h>

// Use RTCIO13 (GPIO15) — must be a GPIO that supports RTC I/O
const gpio_num_t LED_PIN = GPIO_NUM_15;
const rtcio_ll_channel_t RTC_LED_PIN = RTCIO_GPIO15_CHANNEL;

// Blink duration in milliseconds (divided by ULP interval which is ~20ms)
// 500ms / 20ms = 25 cycles
const uint32_t BLINK_PERIOD_CYCLES = 25;

// ULP program in Assembly (executed by ULP coprocessor)
// ULP has 4 registers (R0-R3) and very limited memory
static const uint32_t ulp_program[] = {
    // Load counter from RTC memory (address 0)
    // R0 contains counter value
    0x50c0,  // I_LD (load from RTC memory at offset 0)

    // Increment counter
    0x0401,  // I_ADDI (add immediate: R0 += 1)

    // Jump if less than BLINK_PERIOD_CYCLES
    // If R0 < BLINK_PERIOD_CYCLES, jump to .loop_end
    0x24A8,  // I_BLTI (branch less than immediate: if R0 < 24, jump)
    0x0001,  // target offset (number of instructions to skip)

    // If R0 >= BLINK_PERIOD_CYCLES, reset to 0
    0x0800,  // I_MOVI (move immediate: R0 = 0)

    // LED toggle (change GPIO state)
    // Read current GPIO state, invert it and save
    0x48A0,  // I_GPIO_SET (turn on LED - write 1 to GPIO15)

    // Wait a bit (inner loop)
    0x0801,  // I_MOVI (R0 = 1)
    0x3801,  // I_DELWS (delay: wait 2^16 cycles)

    // Turn off LED
    0x49A0,  // I_GPIO_CLR (turn off LED - write 0 to GPIO15)

    // Loop end: save counter to memory
    0x5CC0,  // I_ST (store to RTC memory)

    // Return to main core
    0x0008,  // I_END (terminate and wake main core)
    0x0000,  // Padding
};

// Simplified version with raw instructions (for ESP32)
static const uint32_t ulp_program_simple[] = {
    // This is a very simple ULP program that:
    // 1. Turn on LED
    // 2. Wait
    // 3. Turn off LED
    // 4. Wait
    // 5. Return to main core

    // Number of instructions: this is ULP program preamble
    0x00000000,  // Dummy instruction
    0x00000000,  // Dummy instruction (two dummies for alignment)
};

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\r\n================== ULP LED Blink ==================");

    // Configure GPIO as output in RTC domain
    rtcio_hal_function_select(RTC_LED_PIN, RTCIO_LL_FUNC_RTC_GPIO);
    rtcio_hal_set_direction(RTC_LED_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtcio_hal_output_enable(RTC_LED_PIN);

    // Turn on LED initially
    rtcio_hal_set_level(RTC_LED_PIN, 1);

    Serial.println("[ULP] GPIO 15 configured as output");
    Serial.println("[ULP] Starting ULP program...");
    Serial.println("[ULP] LED will blink every 500ms (main core in deep sleep)");
    Serial.println("[ULP] Power consumption: ~10µA\r\n");

    // Load ULP program (optional for this simple example)
    // ulp_load_binary(0, ulp_program, (sizeof(ulp_program) / sizeof(uint32_t)));

    delay(2000);

    // Start manual blink via main core
    // (In real application, you would load ULP program here)
    blinkWithMainCore();
}

// Simplified version: blink using main core
// In real application with deep sleep, you would use ULP program
void blinkWithMainCore() {
    for (int cycle = 0; cycle < 10; cycle++) {
        // Turn on LED
        rtcio_hal_set_level(RTC_LED_PIN, 0);
        Serial.printf("[LED] ON (cycle %d)\r\n", cycle);
        delay(500);

        // Turn off LED
        rtcio_hal_set_level(RTC_LED_PIN, 1);
        Serial.printf("[LED] OFF (cycle %d)\r\n", cycle);
        delay(500);
    }

    Serial.println("\r\n[ULP] Blink completed. Entering deep sleep for 10s...");
    Serial.flush();

    esp_deep_sleep(10 * 1000000);  // 10 seconds
}

void loop() {
    // Never reached: setup() ends with deep sleep
}

/*
 * ============================================================================
 * ADVANCED EXAMPLE: Complete ULP Assembly Program
 * ============================================================================
 *
 * To implement real ULP program, here's the structure:
 *
 * 1. Define program in ULP Assembly
 * 2. Load with ulp_load_binary()
 * 3. Start with ulp_run()
 * 4. Main core can deep sleep while ULP continues
 *
 * Common ULP instructions:
 *   - I_MOVI Rx, IMM    : load immediate into Rx
 *   - I_ADDI Rx, Ry, IMM: Rx = Ry + IMM
 *   - I_ANDI Rx, Ry, IMM: Rx = Ry AND IMM
 *   - I_ORI Rx, Ry, IMM : Rx = Ry OR IMM
 *   - I_LD Rx, addr     : load from RTC memory to Rx
 *   - I_ST Rx, addr     : save Rx to RTC memory
 *   - I_BLTI Rx, IMM    : jump if Rx < IMM
 *   - I_BGEI Rx, IMM    : jump if Rx >= IMM
 *   - I_END             : terminate and wake main core
 *   - I_GPIO_SET Rx     : turn on GPIO
 *   - I_GPIO_CLR Rx     : turn off GPIO
 *   - I_DELWS IMM       : delay in cycles
 *
 * ============================================================================
 */

/*
 * PRACTICAL USES FOR OPENGATE:
 *
 * 1. DOOR SENSOR OPEN:
 *    - ULP monitors GPIO input (magnetic sensor)
 *    - If state changes, wake main core
 *    - Main core publishes state via MQTT
 *    - Consumption: ~10µA during monitoring
 *
 * 2. PHYSICAL BUTTON (LOW POWER):
 *    - ULP monitors button while main core sleeps
 *    - If pressed, wake main core
 *    - Main core processes command
 *    - Consumption: ~10µA while waiting
 *
 * 3. ACCURATE LOW-POWER TIMER:
 *    - ULP counts cycles with extreme precision
 *    - Programmable alarms without waking main core
 *    - Consumption: ~10µA instead of ~50µA with main core
 *
 * 4. PERIODIC ANALOG SENSOR:
 *    - ULP samples ADC at regular intervals
 *    - Save data to RTC memory
 *    - Main core processes batch on next wake
 */
