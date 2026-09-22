/*
 * ULP Program for ESP32 — Raw Binary (no Assembly compilation)
 *
 * File: src/ulp_main.c
 *
 * This approach avoids external Assembly compilation.
 * ULP program is defined as array of uint32_t (raw instructions).
 *
 * Operation:
 * 1. Turn on GPIO 15 (LED)
 * 2. Wait ~500ms
 * 3. Turn off GPIO 15
 * 4. Wait ~500ms
 * 5. Loop indefinitely (until halt)
 *
 * Consumption: ~10µA when main core is in deep sleep
 */

#include <esp32/ulp.h>
#include <driver/rtc_io.h>
#include <stdio.h>
#include <string.h>

// ========== ULP Instructions as Macros ==========
//
// ULP has 4 32-bit registers (R0-R3) and a minimalist instruction set.
// Instruction encoding:
//   31:25 = opcode
//   Remaining bits = operands (specific for each instruction)
//

// MOVI Rd, Imm16
// Load a 16-bit immediate into a register
// Opcode: 23 (0x17)
#define MOVI(Rd, Imm) \
    ((23 << 25) | ((Rd) & 0x3) << 23 | ((Imm) & 0xFFFF))

// WR_REG addr, offset, len, data
// Write to RTC register
// Opcode: 12 (0x0C)
// Addr: register address (9-bit)
// Offset: bit offset (5-bit)
// Len: number of bits (4-bit)
// Data: value to write (8-bit)
#define WR_REG(Addr, Offset, Len, Data) \
    ((12 << 25) | ((Addr) & 0x1FF) | (((Offset) & 0x1F) << 9) | (((Len) - 1) & 0xF) << 14 | ((Data) & 0xFF) << 18)

// SUB Rd, Rs, Rt
// Subtract: Rd = Rs - Rt
// Opcode: 4 (0x04)
#define SUB(Rd, Rs, Rt) \
    ((4 << 25) | ((Rd) & 0x3) << 23 | ((Rs) & 0x3) << 21 | ((Rt) & 0x3) << 19)

// SUB Rd, Rs, Imm (SUBI)
// Subtract immediate: Rd = Rs - Imm
// Opcode: 14 (0x0E)
#define SUBI(Rd, Rs, Imm) \
    ((14 << 25) | ((Rd) & 0x3) << 23 | ((Rs) & 0x3) << 21 | ((Imm) & 0xFF) << 13)

// BGEI Rs, Imm, Offset
// Branch if greater or equal to immediate: if Rs >= Imm, branch
// Opcode: 18 (0x12)
#define BGEI(Rs, Imm, Offset) \
    ((18 << 25) | ((Rs) & 0x3) << 23 | ((Imm) & 0xFF) << 13 | ((Offset) & 0x7F) << 6)

// BLTI Rs, Imm, Offset
// Branch if less than immediate: if Rs < Imm, branch
// Opcode: 19 (0x13)
#define BLTI(Rs, Imm, Offset) \
    ((19 << 25) | ((Rs) & 0x3) << 23 | ((Imm) & 0xFF) << 13 | ((Offset) & 0x7F) << 6)

// JMP Offset
// Unconditional jump
// Opcode: 2 (0x02)
#define JMP(Offset) \
    ((2 << 25) | ((Offset) & 0x7FF))

// HALT
// Terminate ULP execution and wake main core
// Opcode: 8 (0x08)
#define HALT() \
    ((8 << 25))

// WAIT Cycles
// Wait N cycles (~10µs per cycle at 100kHz)
// Opcode: 1 (0x01)
#define WAIT(Cycles) \
    ((1 << 25) | ((Cycles) & 0xFFFFFF))

// ========== ULP Program as Array ==========
//
// This is the program that blinks the LED:
//
// 0: MOVI R0, 1                    # R0 = 1
// 1: WR_REG RTC_GPIO_OUT, 29, 1, 1  # Turn on GPIO 15
// 2: WAIT 50000                    # Wait ~500ms
// 3: MOVI R0, 0                    # R0 = 0
// 4: WR_REG RTC_GPIO_OUT, 29, 1, 0  # Turn off GPIO 15
// 5: WAIT 50000                    # Wait ~500ms
// 6: JMP 0                         # Infinite loop
// 7: HALT                          # End (never reached)
//

static const uint32_t ulp_program[] = {
    // Turn on LED
    MOVI(0, 1),                           // 0: R0 = 1
    WR_REG(0x54, 29, 1, 1),               // 1: Turn on GPIO 15 (RTC_GPIO_OUT_REG, bit 29, len 1, val 1)

    // Wait ~500ms
    WAIT(50000),                          // 2: Wait 50000 cycles

    // Turn off LED
    MOVI(0, 0),                           // 3: R0 = 0
    WR_REG(0x54, 29, 1, 0),               // 4: Turn off GPIO 15

    // Wait ~500ms
    WAIT(50000),                          // 5: Wait 50000 cycles

    // Loop
    JMP(0),                               // 6: Jump to instruction 0

    // End
    HALT(),                               // 7: Terminate (never reached)
};

// ========== Load and Run ==========

esp_err_t ulp_load_and_run() {
    // Calculate program size in words
    size_t program_size = sizeof(ulp_program) / sizeof(uint32_t);

    printf("[ULP] Loading program (%d instructions)...\n", program_size);

    // Load program into RTC memory
    esp_err_t err = ulp_load_binary(
        0,                      // load_addr: RTC address to load at
        ulp_program,            // program_binary: instruction array
        program_size            // program_size_words: number of instructions
    );

    if (err != ESP_OK) {
        printf("[ULP] Load error: %d\n", err);
        return err;
    }

    printf("[ULP] Program loaded successfully\n");

    // Start ULP program
    err = ulp_run(0);  // 0 = start address

    if (err != ESP_OK) {
        printf("[ULP] Start error: %d\n", err);
        return err;
    }

    printf("[ULP] Program running\n");
    return ESP_OK;
}

// ========== RTC GPIO Configuration ==========

void ulp_gpio_setup() {
    // Configure GPIO 15 (RTCIO 13) as RTC output
    // GPIO 15 is ideal because:
    // - It's in RTC domain (available in deep sleep)
    // - It's free (not used by MQTT or Servo)
    // - It's easily testable (has pin access)

    printf("[GPIO] Configuring GPIO 15 as RTC output...\n");

    // Select RTC_GPIO function
    rtcio_ll_function_select(RTCIO_GPIO15_CHANNEL, RTCIO_LL_FUNC_RTC_GPIO);

    // Configure direction (output)
    rtcio_ll_set_direction(RTCIO_GPIO15_CHANNEL, RTC_GPIO_MODE_OUTPUT_ONLY);

    // Enable output
    rtcio_ll_output_enable(RTCIO_GPIO15_CHANNEL);

    // Initialize to low (LED off)
    rtcio_ll_set_level(RTCIO_GPIO15_CHANNEL, 0);

    printf("[GPIO] GPIO 15 configured and ready\n");
}

// ========== Main Test Function ==========

void init_ulp_blink() {
    // Setup GPIO
    ulp_gpio_setup();

    // Load and run ULP program
    esp_err_t err = ulp_load_and_run();

    if (err == ESP_OK) {
        printf("[ULP] Setup completed! LED will blink every 500ms\n");
        printf("[ULP] Consumption: ~10µA (main core can deep sleep)\n");
    } else {
        printf("[ULP] Error during initialization\n");
    }
}

// ========== Include this file in your main sketch ==========
//
// In file opengate.ino:
//
// #include "ulp_main.c"
//
// void setup() {
//     ...
//     init_ulp_blink();  // Start ULP blink
//     esp_deep_sleep_enable_ulp_wakeup();
//     esp_deep_sleep(10 * 1000000);  // 10 seconds
// }
//
// void loop() {
//     // Never reached
// }
//
// ========== Parameters ==========
//
// To modify timing, change WAIT value:
//   WAIT(50000) ≈ 500ms    (at 100kHz: 50000 * 10µs)
//   WAIT(100000) ≈ 1 second
//   WAIT(10000) ≈ 100ms
//
// To change GPIO, modify WR_REG:
//   GPIO 15 (bit 29): WR_REG(0x54, 29, 1, X)
//   GPIO 14 (bit 28): WR_REG(0x54, 28, 1, X)
//   GPIO 13 (bit 27): WR_REG(0x54, 27, 1, X)
//   ...and so on
//
// ========== Note on RTC_GPIO_OUT_REG ==========
//
// RTC_GPIO_OUT_REG (address 0x54) controls output of RTC GPIO.
// Bit mapping:
//   Bit 29: GPIO 15
//   Bit 28: GPIO 14
//   Bit 27: GPIO 13
//   ...
//
// To turn on/off a GPIO, write to the corresponding bit.
//
