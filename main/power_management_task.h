/**
 * power_management_task.h
 *
 * This is the one subsystem in the project where "close enough" isn't
 * good enough, so read this before touching power_management_task.c.
 *
 * The THRESHOLDS and SEQUENCE below (temperature zones, the requirement
 * that overheat-shutdown only fires while actually running at meaningful
 * power, the cut-voltage-first shutdown order, the minimum cooldown time,
 * the reduced-settings recovery ramp) are transcribed faithfully from
 * ESP-Miner's own published architecture documentation and are the part
 * of this whole project I'd trust most.
 *
 * What this file does NOT pretend to know: the exact I2C register
 * protocol for whichever voltage regulator and fan/temperature
 * controller chips are actually on the N8-T. TPS546-family regulators
 * and EMC2101-family fan controllers are common choices in this class of
 * board and real datasheet-documented parts, but guessing their register
 * maps from memory and writing that straight to a voltage regulator is
 * exactly the kind of mistake that can over-volt a chip. The functions
 * that would talk to those ICs are deliberately left as clearly-marked
 * stubs in power_management_task.c - confirm the actual parts on your
 * board against their datasheets before filling those in.
 */

#pragma once

#include <stdbool.h>

/* Temperature zones, °C. Source: ESP-Miner power/thermal documentation. */
#define TEMP_THROTTLE_ASIC_C    75.0f
#define TEMP_THROTTLE_VR_C      105.0f
#define TEMP_OVERHEAT_ASIC_C    90.0f   /* upper bound of the documented throttle band */
#define TEMP_OVERHEAT_VR_C      145.0f
#define TEMP_SAFE_RESTART_ASIC_C 45.0f
#define TEMP_SAFE_RESTART_VR_C   95.0f

/* Overheat shutdown only fires while actually running at meaningful
 * power, to avoid repeatedly "detecting" overheat on a system that's
 * already shut itself down. */
#define OVERHEAT_MIN_FREQUENCY_MHZ 50.0f
#define OVERHEAT_MIN_VOLTAGE_MV    1000

#define POWER_POLL_INTERVAL_MS     1800
#define COOLDOWN_MIN_CYCLES        6      /* 6 x 5s = 30s minimum cooldown */
#define COOLDOWN_CYCLE_MS          5000

#define RECOVERY_VOLTAGE_STEP_MV   100
#define RECOVERY_VOLTAGE_FLOOR_MV  1000
#define RECOVERY_FREQUENCY_STEP_MHZ 100.0f
#define RECOVERY_FREQUENCY_FLOOR_MHZ 400.0f

void power_management_task(void *pvParameters);
