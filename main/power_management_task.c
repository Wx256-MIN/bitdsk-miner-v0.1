/**
 * power_management_task.c — see header for what's verified vs. stubbed.
 */

#include "power_management_task.h"
#include "global_state.h"
#include "board_config.h"
#include "bm1397.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"

static const char *TAG = "power_mgmt";

/* ===================================================================
 * SENSOR / REGULATOR I/O — STUBS. See the header's confidence note.
 * Each function below logs what it *would* do and returns a plausible
 * placeholder value so the rest of the state machine can be exercised
 * and reasoned about, but none of these actually speak to real silicon
 * yet. Fill these in against your board's actual regulator/fan-
 * controller datasheets - do not guess register addresses for the
 * voltage regulator specifically; an over-voltage write is the one
 * mistake in this whole project that can destroy the ASIC outright.
 * ================================================================= */

static bool vcore_set_voltage_mv(int mv)
{
    ESP_LOGW(TAG, "[STUB] would set core voltage to %d mV via I2C addr 0x%02X - "
                   "implement against your regulator's real datasheet before trusting this",
             mv, I2C_ADDR_VCORE_REGULATOR);
    /* Returning true here only unblocks the surrounding logic for
     * development/reading purposes - a stub that always "succeeds"
     * must not be mistaken for a working voltage rail. */
    return true;
}

static float read_input_voltage_mv(void)   { return 5000.0f; /* STUB */ }
static float read_input_current_ma(void)   { return 0.0f;    /* STUB */ }
static float read_input_power_w(void)      { return 0.0f;    /* STUB */ }
static float read_vr_temp_c(void)          { return 25.0f;   /* STUB */ }

/* Real boards may read ASIC temperature from a dedicated thermal diode
 * (only valid while the chip is powered) or from an external sensor
 * near the chip. Returning -1 here mirrors the documented sentinel for
 * "reading unavailable" so the rest of the logic already handles it
 * correctly once a real implementation replaces this. */
static float read_asic_temp_c(void)        { return -1.0f;  /* STUB */ }

static bool set_fan_percent(float percent)
{
    ESP_LOGI(TAG, "[STUB] would set fan to %.0f%% via I2C addr 0x%02X",
             percent, I2C_ADDR_FAN_CONTROLLER);
    return true;
}

static void asic_hold_reset(bool hold)
{
    gpio_set_level(PIN_ASIC_RESET, hold ? 0 : 1); /* active-low assumption - verify */
}

/* =================================================================== */

static bool is_overheat(float chip_temp, float vr_temp, float freq_mhz, int voltage_mv)
{
    bool temp_bad = (chip_temp > TEMP_OVERHEAT_ASIC_C) || (vr_temp > TEMP_OVERHEAT_VR_C);
    bool meaningful_power = (freq_mhz > OVERHEAT_MIN_FREQUENCY_MHZ) || (voltage_mv > OVERHEAT_MIN_VOLTAGE_MV);
    return temp_bad && meaningful_power;
}

static void emergency_shutdown(void)
{
    ESP_LOGE(TAG, "OVERHEAT DETECTED - emergency shutdown");

    /* 1. Cut core voltage immediately. */
    vcore_set_voltage_mv(0);
    /* 2. Hold ASIC in reset to minimize power draw. */
    asic_hold_reset(true);
    /* 3. Persist current settings so recovery can compute reduced values. */
    nvs_config_save_operating_point(g_state.current_frequency_mhz, g_state.current_voltage_mv);
    /* 4. Fan to full speed regardless of the normal fan curve. */
    set_fan_percent(100.0f);
    /* 5. Record overheat mode so it survives a reboot until cleared. */
    nvs_config_set_overheat_mode(true);

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.overheat_mode = true;
    g_state.asic_initialized = false;
    xSemaphoreGive(g_state.mutex);
}

static void cooling_wait_loop(bool asic_temp_valid)
{
    int cycles = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_CYCLE_MS));
        cycles++;

        float vr_temp = read_vr_temp_c();
        float chip_temp = asic_temp_valid ? read_asic_temp_c() : -1.0f;

        bool vr_ok = vr_temp < TEMP_SAFE_RESTART_VR_C;
        bool chip_ok = asic_temp_valid ? (chip_temp < TEMP_SAFE_RESTART_ASIC_C) : true;

        if (!vr_ok || (asic_temp_valid && !chip_ok)) {
            ESP_LOGW(TAG, "still cooling: vr=%.1fC chip=%.1fC (cycle %d)", vr_temp, chip_temp, cycles);
            cycles = 0; /* documented behavior: reset the counter if still too hot */
            continue;
        }

        if (cycles >= COOLDOWN_MIN_CYCLES) {
            ESP_LOGI(TAG, "cooled down after %d cycles (vr=%.1fC chip=%.1fC)", cycles, vr_temp, chip_temp);
            return;
        }
    }
}

static void recover_from_overheat(void)
{
    int new_voltage = g_state.current_voltage_mv - RECOVERY_VOLTAGE_STEP_MV;
    if (new_voltage < RECOVERY_VOLTAGE_FLOOR_MV) new_voltage = RECOVERY_VOLTAGE_FLOOR_MV;

    float new_freq = g_state.current_frequency_mhz - RECOVERY_FREQUENCY_STEP_MHZ;
    if (new_freq < RECOVERY_FREQUENCY_FLOOR_MHZ) new_freq = RECOVERY_FREQUENCY_FLOOR_MHZ;

    nvs_config_save_operating_point(new_freq, new_voltage);

    vcore_set_voltage_mv(new_voltage);
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the rail stabilize before touching the ASIC */

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.asic_initialized = false;
    xSemaphoreGive(g_state.mutex);
    vTaskDelay(pdMS_TO_TICKS(500)); /* let mining tasks notice and stop touching UART */

    asic_hold_reset(false);
    vTaskDelay(pdMS_TO_TICKS(2000)); /* stabilization delay before re-init, per documented recovery flow */

    bool ok = bm1397_init(ASIC_CHIP_COUNT, new_freq, (uint16_t)new_voltage);

    xSemaphoreTake(g_state.mutex, portMAX_DELAY);
    g_state.asic_initialized = ok;
    g_state.current_frequency_mhz = new_freq;
    g_state.current_voltage_mv = new_voltage;
    if (ok) {
        g_state.overheat_mode = false;
        nvs_config_set_overheat_mode(false);
    }
    xSemaphoreGive(g_state.mutex);

    ESP_LOGI(TAG, "recovery %s: freq=%.0fMHz voltage=%dmV", ok ? "OK" : "FAILED", new_freq, new_voltage);
}

void power_management_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "power_management_task started (priority 10, poll every %dms)", POWER_POLL_INTERVAL_MS);

    gpio_set_direction(PIN_ASIC_RESET, GPIO_MODE_OUTPUT);

    /* Track whether this board can read a live ASIC temperature (some
     * designs only get a valid reading while the chip is powered, per
     * the documented distinction between boards with a real thermal
     * sensor vs. ones relying solely on the ASIC's internal diode). */
    bool asic_temp_valid = true;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_INTERVAL_MS));

        float chip_temp = read_asic_temp_c();
        float vr_temp = read_vr_temp_c();
        float voltage = read_input_voltage_mv();
        float current = read_input_current_ma();
        float power = read_input_power_w();

        if (chip_temp < -0.5f || chip_temp > 126.0f) {
            asic_temp_valid = false; /* -1 or 127 sentinel = sensor unavailable/open circuit */
        }

        xSemaphoreTake(g_state.mutex, portMAX_DELAY);
        g_state.chip_temp_c = chip_temp;
        g_state.vr_temp_c = vr_temp;
        g_state.input_voltage_mv = voltage;
        g_state.current_ma = current;
        g_state.power_watts = power;
        float freq = g_state.current_frequency_mhz;
        int core_mv = g_state.current_voltage_mv;
        bool already_overheated = g_state.overheat_mode;
        xSemaphoreGive(g_state.mutex);

        if (already_overheated) {
            continue; /* recovery flow below owns clearing this flag */
        }

        if (is_overheat(chip_temp, vr_temp, freq, core_mv)) {
            emergency_shutdown();
            cooling_wait_loop(asic_temp_valid);
            recover_from_overheat();
        }
    }
}
