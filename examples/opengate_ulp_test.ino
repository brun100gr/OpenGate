/*
 * OpenGate — ULP LED Blink — READY TO USE (no Assembly)
 *
 * File: src/opengate_ulp_test.ino
 *
 * This sketch does not require external Assembly compilation.
 * The ULP program is a simple array of uint32_t in C.
 *
 * Hardware:
 *   - LED: GPIO 15 with 330Ω resistor
 *   - GND
 *
 * To test:
 *   1. Upload this sketch
 *   2. Connect LED to GPIO 15
 *   3. Press reset
 *   4. See LED blinking for 10 seconds
 *   5. Main core sleeps (~10µA consumption)
 */

#include <esp32/ulp.h>
#include <driver/rtc_io.h>
#include <stdio.h>

// ========== ULP Program as raw instructions ==========
// No Assembly compilation required!

// Macros for ULP instructions
#define MOVI(Rd, Imm) \
    ((23 << 25) | ((Rd) & 0x3) << 23 | ((Imm) & 0xFFFF))

#define WR_REG(Addr, Offset, Len, Data) \
    ((12 << 25) | ((Addr) & 0x1FF) | (((Offset) & 0x1F) << 9) | (((Len) - 1) & 0xF) << 14 | ((Data) & 0xFF) << 18)

#define WAIT(Cycles) \
    ((1 << 25) | ((Cycles) & 0xFFFFFF))

#define JMP(Offset) \
    ((2 << 25) | ((Offset) & 0x7FF))

#define HALT() \
    ((8 << 25))

// Program: turn on LED, wait 500ms, turn off, wait, loop
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

// ========== Setup ==========

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\r\n================== OpenGate ULP Blink ==================\r\n");

    // Step 1: Configure GPIO 15
    Serial.println("[GPIO] Configuring GPIO 15 as RTC output...");
    rtcio_ll_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);
    rtcio_ll_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtcio_ll_output_enable(RTCIO_GPIO15_CHANNEL);
    rtcio_ll_set_level(RTCIO_GPIO15_CHANNEL, 0);
    Serial.println("[GPIO] ✓ GPIO 15 configured\n");

    // Step 2: Load ULP program
    Serial.println("[ULP] Loading program...");
    size_t program_size = sizeof(ulp_program) / sizeof(uint32_t);

    esp_err_t err = ulp_load_binary(0, ulp_program, program_size);
    if (err != ESP_OK) {
        Serial.printf("[ULP] Load error: %d\n", err);
        return;
    }
    Serial.println("[ULP] ✓ Program loaded\n");

    // Step 3: Start ULP
    Serial.println("[ULP] Starting program...");
    err = ulp_run(0);
    if (err != ESP_OK) {
        Serial.printf("[ULP] Start error: %d\n", err);
        return;
    }
    Serial.println("[ULP] ✓ Program running\n");

    // Step 4: Info
    Serial.println("Behavior:");
    Serial.println("  - LED blinks every 500ms");
    Serial.println("  - Main core sleeps for 10 seconds");
    Serial.println("  - Consumption: ~10µA\n");

    delay(2000);

    // Step 5: Deep sleep (LED continues blinking via ULP)
    Serial.println("[SLEEP] Entering deep sleep for 10 seconds...");
    Serial.flush();

    esp_deep_sleep(10 * 1000000ULL);  // 10 seconds
}

void loop() {
    // Never reached
}
