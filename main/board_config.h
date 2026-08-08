/**
 * board_config.h
 *
 * ============================================================================
 *  READ THIS BEFORE YOU DO ANYTHING ELSE.
 * ============================================================================
 *
 * Every GPIO number below is a PLACEHOLDER. BitDsk has not published a
 * schematic, pinout, or GPIO map for the N8-T, so nobody outside BitDsk's
 * own firmware team currently has a verified pin mapping for this exact
 * board. The numbers here are just plausible ESP32 pin choices so the rest
 * of the project has something to compile against — they are NOT derived
 * from your hardware and WILL be wrong until you replace them.
 *
 * Flashing this firmware onto a real N8-T with unverified pins can, at
 * minimum, mean nothing works, and at worst means driving a GPIO into a
 * net it was never meant to touch. Don't power up the ASIC (i.e. don't
 * call BM1397_init / VCORE_set_voltage) until every pin below has been
 * physically verified.
 *
 * How to actually determine these values, roughly in order of how much
 * you can learn without destructive disassembly:
 *
 *   1. Identify the exact ESP32 module printed on the board (e.g.
 *      "ESP32-S3-WROOM-1"). This tells you which physical pins exist and
 *      which are input-only / strapping pins to avoid.
 *   2. Dump the STOCK firmware's flash over USB with esptool.py
 *      (`esptool.py read_flash 0 0x400000 backup.bin`) before you change
 *      anything. This gives you a fallback to restore if something goes
 *      wrong, and the binary can sometimes be inspected (strings, IDF
 *      partition table) for clues about which components/pins are in use.
 *   3. With the board powered off, use a multimeter in continuity/diode
 *      mode to trace from each ESP32 module pin to: the BM1397's UART-ish
 *      TX/RX-looking pads, any chip with an I2C-shaped 2-wire + power/gnd
 *      footprint (voltage regulator, fan controller), the fan connector,
 *      and any reset/enable-looking test points.
 *   4. Probe the actual UART lines with a logic analyzer while the STOCK
 *      firmware is running — BM1397 traffic at boot is distinctive
 *      (repeated framed packets at 115200 baud initially) and will tell
 *      you definitively which two pins are the ASIC UART, which removes
 *      the biggest unknown.
 *
 * Until you've done that, treat this entire project as a reference
 * architecture to read, not firmware to flash.
 */

#pragma once

#include "driver/gpio.h"

/* ---- ASIC (BM1397) UART -------------------------------------------------
 * BM1397 logic is 1.8V; most of these boards level-shift to the ESP32's
 * 3.3V UART pins. Confirm level shifting exists before assuming you can
 * wire directly to a bare BM1397 pad.
 */
#define PIN_ASIC_UART_TX      GPIO_NUM_17   /* TODO: VERIFY */
#define PIN_ASIC_UART_RX      GPIO_NUM_18   /* TODO: VERIFY */
#define ASIC_UART_PORT         UART_NUM_1
#define ASIC_UART_BAUD_INITIAL 115200        /* BM1397 default power-on baud */
#define ASIC_UART_BAUD_FAST    3125000       /* documented BM1397 max; drop to 1000000 if the link is unstable */

/* ---- ASIC power / reset --------------------------------------------------
 * Some boards gate ASIC power through a discrete enable pin in addition to
 * the regulator itself; others rely solely on the regulator's own enable.
 * Leave PIN_ASIC_ENABLE as -1 if your board doesn't have one.
 */
#define PIN_ASIC_RESET         GPIO_NUM_1    /* TODO: VERIFY - active low is typical */
#define PIN_ASIC_ENABLE        (-1)          /* TODO: VERIFY - set to a real GPIO if applicable */

/* ---- I2C bus (voltage regulator + fan/temp controller) ------------------
 * ESP-Miner boards commonly use a TPS546-family buck regulator for ASIC
 * core voltage and an EMC2101/2302-family part for fan PWM + temperature.
 * Both are I2C. Confirm which parts (if any) are actually on this board -
 * BitDsk may use different ICs entirely.
 */
#define PIN_I2C_SDA             GPIO_NUM_4    /* TODO: VERIFY */
#define PIN_I2C_SCL             GPIO_NUM_5    /* TODO: VERIFY */
#define I2C_PORT                I2C_NUM_0
#define I2C_CLOCK_HZ             400000

#define I2C_ADDR_VCORE_REGULATOR  0x24  /* TODO: VERIFY - TPS546 family default; confirm on your board */
#define I2C_ADDR_FAN_CONTROLLER   0x4C  /* TODO: VERIFY - EMC2101 family default; confirm on your board */

/* ---- Fan (only used if the board drives the fan directly rather than
 * through an I2C fan controller) ------------------------------------------
 */
#define PIN_FAN_PWM              GPIO_NUM_6    /* TODO: VERIFY, or ignore if using I2C fan controller */
#define PIN_FAN_TACH             GPIO_NUM_7    /* TODO: VERIFY */

/* ---- Status LED / button (optional, adjust to match your board) -------- */
#define PIN_STATUS_LED           GPIO_NUM_8    /* TODO: VERIFY, or -1 if none */
#define PIN_BOOT_BUTTON          GPIO_NUM_0    /* usually the standard ESP32 BOOT button */

/* ---- ASIC chain configuration -------------------------------------------
 * How many BM1397 chips are wired in series on this board. The N8-T's
 * "N8" naming and ~200 GH/s rating are consistent with a single BM1397
 * (a single chip in a Bitaxe-class device runs in roughly this range),
 * but this is an inference from the marketing hashrate, not a verified
 * fact - confirm by counting chips on the board itself.
 */
#define ASIC_CHIP_COUNT           1     /* TODO: VERIFY by physically counting chips on the board */
#define ASIC_SMALL_CORE_COUNT     672   /* TODO: VERIFY - see README for why this number is uncertain for BM1397 */

/* ---- Default operating point --------------------------------------------
 * Conservative defaults, deliberately lower than what stock firmware
 * typically ships with. Raise only after you've confirmed cooling is
 * adequate and voltage/frequency control is actually working correctly -
 * see power_management_task.c for the safety logic that governs these.
 */
#define ASIC_DEFAULT_FREQUENCY_MHZ   400.0f  /* stock BM1397 designs commonly run 400-490 MHz */
#define ASIC_DEFAULT_VOLTAGE_MV      1350    /* stock BM1397 designs commonly run ~1300-1400 mV */
