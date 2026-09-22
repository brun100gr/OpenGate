/*
 * ULP Program for ESP32 — Low-power LED Blink
 *
 * File: src/ulp/ulp_blink.S
 *
 * NOTE: This file is for documentation. To compile ULP code,
 * use one of these methods:
 *
 * METHOD A (RECOMMENDED): Raw binary embedded in C
 *   - File: src/ulp_main.c
 *   - ULP instructions as array of uint32_t
 *   - Does not require external Assembly compilation
 *   - More reliable and portable
 *
 * METHOD B: ESP-IDF ULP Toolchain
 *   - Requires: esp-idf + esp-idf-tools
 *   - Command: idf.py ulp-build
 *   - Output: binary .bin
 *
 * THIS FILE (Pure Assembly) requires correct ULP syntax
 * which differs from standard Xtensa. For now we use METHOD A.
 */

// ============================================================================
// CORRECT ULP ASSEMBLY SYNTAX (for reference)
// ============================================================================
//
// This is the CORRECT syntax to use if you compile with idf.py ulp-build:
//
// .global entry
// entry:
//     MOVI r0, 1
//     WR_REG RTC_GPIO_OUT_REG, 29, 1, 1   # Turn on GPIO 15
//
//     MOVI r0, 50000
// wait_on:
//     SUB r0, r0, 1
//     BGEI r0, 1, wait_on                  # If r0 >= 1, continue loop
//
//     MOVI r0, 0
//     WR_REG RTC_GPIO_OUT_REG, 29, 1, 0   # Turn off GPIO 15
//
//     MOVI r0, 50000
// wait_off:
//     SUB r0, r0, 1
//     BGEI r0, 1, wait_off
//
//     JMP entry
//     HALT
//
// NOTE: Different from standard Assembly syntax!
//   - Registers: R0, R1, R2, R3 (UPPERCASE)
//   - Instructions: MOVI, WR_REG, SUB, BGEI, JMP, HALT (UPPERCASE)
//   - No $ prefix before registers
//   - Syntax: INSTRUCTION regdest, regsrc1, regsrc2/imm
//
// ============================================================================

// ============================================================================
// ALTERNATIVE VERSION: With counter in RTC memory
// ============================================================================
//
// If you want to save state between cycles (e.g. blink count), use RTC
// memory which persists even in deep sleep.
//
// entry:
//     // Load counter from RTC memory (offset 0)
//     ld r0, r0, 0
//     addi r0, r0, 1
//     st r0, r0, 0
//
//     // Turn on LED
//     movi r1, 1
//     wr_reg RTC_GPIO_OUT_REG, 29, 1, 1
//     movi r2, 50000
// wait_on:
//     subi r2, r2, 1
//     bgei r2, 1, wait_on
//
//     // Turn off LED
//     movi r1, 0
//     wr_reg RTC_GPIO_OUT_REG, 29, 1, 0
//     movi r2, 50000
// wait_off:
//     subi r2, r2, 1
//     bgei r2, 1, wait_off
//
//     // If counter reached, e.g., 10, exit
//     ld r0, r0, 0
//     blti r0, 10, entry                       // If < 10, continue
//     halt                                      // Otherwise terminate

// ============================================================================
// AVAILABLE ULP INSTRUCTIONS
// ============================================================================
//
// Data movement:
//   movi Rd, Imm16              # Rd = Imm16
//   ld Rd, Offset               # Load from RTC memory
//   st Rd, Offset               # Save to RTC memory
//
// Arithmetic/Logic:
//   addi Rd, Rs, Imm8           # Rd = Rs + Imm8
//   subi Rd, Rs, Imm8           # Rd = Rs - Imm8
//   andi Rd, Rs, Imm8           # Rd = Rs AND Imm8
//   ori Rd, Rs, Imm8            # Rd = Rs OR Imm8
//   xori Rd, Rs, Imm8           # Rd = Rs XOR Imm8
//
// Jumps/Branching:
//   jmp Label                   # Jump to Label
//   beq Rs, Rt, Label           # If Rs == Rt, jump
//   bne Rs, Rt, Label           # If Rs != Rt, jump
//   blti Rs, Imm, Label         # If Rs < Imm, jump
//   bgei Rs, Imm, Label         # If Rs >= Imm, jump
//
// GPIO IO:
//   wr_reg Reg, BitStart, BitLen, Data  # Write to RTC register
//   rd_reg Reg, BitStart, BitLen        # Read from RTC register
//   gpio_set Bit                        # Turn on GPIO
//   gpio_clr Bit                        # Turn off GPIO
//
// Delay:
//   wait Cycles                 # Wait N cycles (~10µs per cycle)
//
// Control:
//   halt                        # Terminate ULP and wake main core
//   sleep Cycles                # Sleep N cycles (low power)
//
// ============================================================================
// RTC GPIO (RTCIO) - Mapping
// ============================================================================
//
// GPIO 0  = RTCIO 0   (pin 11)
// GPIO 2  = RTCIO 2   (pin 24)
// GPIO 4  = RTCIO 4   (pin 23)
// GPIO 12 = RTCIO 12  (pin 9)
// GPIO 13 = RTCIO 13  (pin 10)
// GPIO 14 = RTCIO 14  (pin 5)
// GPIO 15 = RTCIO 15  (pin 6)      <- Recommended for OpenGate
// GPIO 25 = RTCIO 25  (pin 8)
// GPIO 26 = RTCIO 26  (pin 7)
// GPIO 27 = RTCIO 27  (pin 3)
// GPIO 32-39 = RTCIO 0-7 (analog pins)
//
// ============================================================================
// IMPORTANT RTC REGISTERS
// ============================================================================
//
// RTC_GPIO_OUT_REG        = 0x54  # GPIO output (write)
// RTC_GPIO_IN_REG         = 0x55  # GPIO input (read-only)
// RTC_GPIO_ENABLE_REG     = 0x56  # Enable GPIO output
// RTC_GPIO_ENABLE_W1TS    = 0x57  # Enable write set
// RTC_GPIO_ENABLE_W1TC    = 0x58  # Enable write clear
//
// Bit mapping:
// Bit 29: GPIO 15
// Bit 28: GPIO 14
// Bit 27: GPIO 13
// ...and so on
//
// ============================================================================
// POWER CONSUMPTION
// ============================================================================
//
// Main core (active):     ~50-80mA
// Main core (deep sleep): ~50µA
// ULP (active):           ~10µA
//
// With ULP blinking every 500ms:
// - Deep sleep time: ~999ms
// - Active time (per blink): ~1ms
// - Average consumption: 11µA (~80% savings)
//
// ============================================================================
