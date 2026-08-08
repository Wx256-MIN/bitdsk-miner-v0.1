/**
 * bm1397.c — see bm1397.h for the confidence note on which parts of this
 * file are architecturally verified vs. this project's own best-effort
 * reconstruction of undocumented byte-level details.
 */

#include <string.h>
#include "bm1397.h"
#include "serial.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bm1397";
static uint8_t s_next_job_id = 0;
static int s_detected_chips = 0;

/* ===================== CRC implementations =====================
 * See the confidence note in bm1397.h - these are original, from-first-
 * principles implementations of a CRC5 (poly 0x05) and a CRC16/CCITT-FALSE
 * (poly 0x1021, init 0xFFFF), which are the two most common variants used
 * across the BM13xx chip family per community reverse-engineering. Verify
 * against real chip responses before trusting them.
 */
uint8_t crc5_bm1397(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x1F;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x10) {
                crc = ((crc << 1) ^ 0x05) & 0x1F;
            } else {
                crc = (crc << 1) & 0x1F;
            }
        }
    }
    return crc & 0x1F;
}

uint16_t crc16_bm1397(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ===================== Low-level packet send ===================== */

static bool send_cmd_packet(uint8_t command, bool broadcast, uint8_t chip_addr,
                             uint8_t reg, const uint8_t *value, uint8_t value_len)
{
    uint8_t packet[16];
    int idx = 0;
    packet[idx++] = BM1397_PREAMBLE_0;
    packet[idx++] = BM1397_PREAMBLE_1;
    packet[idx++] = BM1397_TYPE_CMD | (broadcast ? BM1397_GROUP_ALL : BM1397_GROUP_SINGLE) | command;
    packet[idx++] = (uint8_t)(value_len + 3); /* +3 = chip_addr + reg + crc5, per documented CMD_PACKET formula */
    packet[idx++] = chip_addr;
    packet[idx++] = reg;
    if (value && value_len) {
        memcpy(&packet[idx], value, value_len);
        idx += value_len;
    }

    uint8_t crc = crc5_bm1397(&packet[2], idx - 2); /* header through last data byte */
    packet[idx++] = crc;

    int written = serial_write(ASIC_UART_PORT, packet, idx);
    return written == idx;
}

static bool write_register(uint8_t chip_addr, uint8_t reg, uint32_t value, bool broadcast)
{
    uint8_t v[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)(value)
    };
    return send_cmd_packet(BM1397_CMD_WRITE, broadcast, chip_addr, reg, v, 4);
}

static bool read_register(uint8_t chip_addr, uint8_t reg, bool broadcast)
{
    return send_cmd_packet(BM1397_CMD_READ, broadcast, chip_addr, reg, NULL, 0);
}

/* ===================== PLL / frequency ===================== */

typedef struct {
    uint16_t fbdiv;
    uint8_t  refdiv;
    uint8_t  postdiv1;
    uint8_t  postdiv2;
    float    actual_mhz;
} pll_params_t;

/* Solves frequency = (25MHz * fbdiv) / (refdiv * postdiv1 * postdiv2) for
 * the combination closest to target_mhz. The divider bounds here are
 * typical ranges for this PLL family, not chip-verified limits - if your
 * chip rejects a computed value, narrow these. */
static bool pll_find_parameters(float target_mhz, pll_params_t *out)
{
    const float FREF = 25.0f;
    float best_err = 1e9f;
    bool found = false;

    for (uint8_t refdiv = 1; refdiv <= 2; refdiv++) {
        for (uint8_t postdiv1 = 1; postdiv1 <= 7; postdiv1++) {
            for (uint8_t postdiv2 = 1; postdiv2 <= postdiv1; postdiv2++) {
                float fbdiv_f = (target_mhz * refdiv * postdiv1 * postdiv2) / FREF;
                uint16_t fbdiv = (uint16_t)(fbdiv_f + 0.5f);
                if (fbdiv < 16 || fbdiv > 511) continue;
                float actual = (FREF * fbdiv) / (refdiv * postdiv1 * postdiv2);
                float err = (actual > target_mhz) ? (actual - target_mhz) : (target_mhz - actual);
                if (err < best_err) {
                    best_err = err;
                    out->fbdiv = fbdiv;
                    out->refdiv = refdiv;
                    out->postdiv1 = postdiv1;
                    out->postdiv2 = postdiv2;
                    out->actual_mhz = actual;
                    found = true;
                }
            }
        }
    }
    return found;
}

static bool apply_frequency_immediate(float mhz)
{
    pll_params_t pll;
    if (!pll_find_parameters(mhz, &pll)) {
        ESP_LOGE(TAG, "no PLL solution near %.1f MHz", mhz);
        return false;
    }
    /* Packing convention for the 4-byte PLL register: this project's own
     * choice (fbdiv in the low 9 bits, then refdiv/postdiv1/postdiv2) -
     * confirm against real chip behavior, adjust if frequency comes out
     * wrong on a scope/logic analyzer. */
    uint32_t reg_val = ((uint32_t)pll.postdiv2 << 24) | ((uint32_t)pll.postdiv1 << 16) |
                        ((uint32_t)pll.refdiv << 12) | (pll.fbdiv & 0x1FF);
    bool ok = write_register(0, BM1397_REG_PLL0_PARAMETER, reg_val, true);
    ESP_LOGI(TAG, "PLL -> target %.1f MHz, actual %.2f MHz (fbdiv=%u refdiv=%u pd1=%u pd2=%u)",
             mhz, pll.actual_mhz, pll.fbdiv, pll.refdiv, pll.postdiv1, pll.postdiv2);
    return ok;
}

bool bm1397_set_frequency(float target_mhz)
{
    return apply_frequency_immediate(target_mhz);
}

/* Ramps up from a low, safe starting point in small steps rather than
 * jumping straight to the target - mirrors the gradual-ramp approach
 * ESP-Miner's docs describe (start low, step up, pause between steps) to
 * avoid destabilizing the PLL or drawing a sudden current spike. */
static bool ramp_frequency(float target_mhz)
{
    const float START_MHZ = 50.0f;
    const float STEP_MHZ  = 25.0f;

    if (target_mhz <= START_MHZ) {
        return apply_frequency_immediate(target_mhz);
    }

    for (float f = START_MHZ; f < target_mhz; f += STEP_MHZ) {
        if (!apply_frequency_immediate(f)) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return apply_frequency_immediate(target_mhz);
}

/* ===================== Chip discovery / addressing ===================== */

int bm1397_detect_chips(int expected_chip_count)
{
    serial_flush(ASIC_UART_PORT);
    read_register(0, BM1397_REG_CHIP_ADDRESS, true);

    int found = 0;
    for (int i = 0; i < expected_chip_count; i++) {
        uint8_t buf[9];
        int n = serial_read(ASIC_UART_PORT, buf, sizeof(buf), 200);
        if (n >= 6 && buf[0] == BM1397_EXPECTED_CHIP_ID_HI && buf[1] == BM1397_EXPECTED_CHIP_ID_LO) {
            found++;
        } else if (n <= 0) {
            break; /* no more chips responding */
        }
    }
    s_detected_chips = found;
    ESP_LOGI(TAG, "chip discovery: expected %d, found %d", expected_chip_count, found);
    return found;
}

static void assign_chip_addresses(int chip_count)
{
    if (chip_count <= 0) return;
    uint8_t interval = (uint8_t)(256 / chip_count);
    for (int i = 0; i < chip_count; i++) {
        uint8_t addr = (uint8_t)(i * interval);
        uint8_t payload[2] = { addr, 0x00 };
        send_cmd_packet(BM1397_CMD_SETADDRESS, false, addr, 0x00, payload, 2);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "assigned %d chip address(es), interval=%u", chip_count, interval);
}

/* ===================== Public init ===================== */

bool bm1397_init(int expected_chip_count, float initial_freq_mhz, uint16_t initial_voltage_mv)
{
    (void)initial_voltage_mv; /* core voltage is the regulator's job - see power_management_task.c */

    ESP_LOGI(TAG, "resetting ASIC chain");
    /* Caller is expected to have already toggled PIN_ASIC_RESET low-then-
     * high with the regulator at a safe voltage before calling this -
     * bm1397_init() only speaks the UART protocol, it doesn't own reset
     * timing or power sequencing (see power_management_task.c). */

    serial_init(ASIC_UART_PORT, PIN_ASIC_UART_TX, PIN_ASIC_UART_RX, ASIC_UART_BAUD_INITIAL);
    vTaskDelay(pdMS_TO_TICKS(100));

    int found = bm1397_detect_chips(expected_chip_count);
    if (found == 0) {
        ESP_LOGE(TAG, "no chips detected - check UART wiring/pins in board_config.h before assuming the driver is broken");
        return false;
    }

    assign_chip_addresses(found);

    /* Baseline register configuration. Values are this project's best
     * approximation of a sane BM1397 bring-up sequence; treat every
     * register write here as something to confirm against measured
     * chip behavior (e.g. does hashrate telemetry start incrementing?)
     * rather than assume is correct on first try. */
    write_register(0, BM1397_REG_MISC_CONTROL, 0xF0000000, true);
    write_register(0, BM1397_REG_CORE_REG_CONTROL, 0x80008B00, true);
    write_register(0, BM1397_REG_TICKET_MASK, 0x00000000, true); /* start unfiltered - see bm1397_set_difficulty_mask */
    write_register(0, BM1397_REG_HASH_COUNTING, 0x00001EB5, true);

    if (!ramp_frequency(initial_freq_mhz)) {
        ESP_LOGE(TAG, "frequency ramp failed");
        return false;
    }

    /* Switch to the faster operating baud once chips are configured -
     * some designs need a short settle delay here; adjust if the link
     * goes silent after this call. */
    vTaskDelay(pdMS_TO_TICKS(50));
    serial_set_baud(ASIC_UART_PORT, ASIC_UART_BAUD_FAST);

    ESP_LOGI(TAG, "BM1397 init complete: %d chip(s) at %.1f MHz", found, initial_freq_mhz);
    return true;
}

bool bm1397_set_difficulty_mask(uint32_t difficulty)
{
    /* Chip-level "ticket mask" filters which internally-found results are
     * even reported over UART, independent of pool difficulty - it exists
     * to keep the serial link from being flooded at high hashrate. This
     * is a simplified approximation (nearest power-of-two below the
     * requested difficulty, expressed as a bitmask); for initial bring-up
     * it's reasonable to leave this at 0 (report everything) and only
     * tighten it once you can see real nonce traffic to calibrate against. */
    uint32_t mask = 0;
    uint32_t d = difficulty;
    while (d > 1) { mask = (mask << 1) | 1; d >>= 1; }
    return write_register(0, BM1397_REG_TICKET_MASK, mask, true);
}

void bm1397_request_hashrate_registers(void)
{
    read_register(0, BM1397_REG_HASH_RATE, true);
    read_register(0, BM1397_REG_ERROR_COUNT, true);
}

bool bm1397_send_job(const bm1397_job_t *job)
{
    uint8_t packet[64];
    int idx = 0;
    packet[idx++] = BM1397_PREAMBLE_0;
    packet[idx++] = BM1397_PREAMBLE_1;
    packet[idx++] = BM1397_TYPE_JOB | BM1397_GROUP_ALL;
    int len_pos = idx;
    packet[idx++] = 0; /* filled in below */
    packet[idx++] = job->job_id;
    packet[idx++] = 1; /* number of midstates in this packet - no midstate rolling */
    memcpy(&packet[idx], job->midstate, 32); idx += 32;
    memcpy(&packet[idx], job->merkle_root_tail, 4); idx += 4;
    memcpy(&packet[idx], job->ntime, 4); idx += 4;
    memcpy(&packet[idx], job->nbits, 4); idx += 4;
    packet[idx++] = (uint8_t)(job->starting_nonce >> 24);
    packet[idx++] = (uint8_t)(job->starting_nonce >> 16);
    packet[idx++] = (uint8_t)(job->starting_nonce >> 8);
    packet[idx++] = (uint8_t)(job->starting_nonce);

    const uint8_t payload_len = 32 + 4 + 4 + 4 + 4; /* 48 */
    packet[len_pos] = payload_len + 4; /* +4 = job_id + num_midstates + crc16 */

    uint16_t crc = crc16_bm1397(&packet[2], idx - 2);
    packet[idx++] = (uint8_t)(crc >> 8);
    packet[idx++] = (uint8_t)(crc);

    return serial_write(ASIC_UART_PORT, packet, idx) == idx;
}

#define BM1397_RESPONSE_LEN 7

bool bm1397_read_response(bm1397_response_t *out, uint32_t timeout_ms)
{
    uint8_t buf[BM1397_RESPONSE_LEN];
    int n = serial_read(ASIC_UART_PORT, buf, BM1397_RESPONSE_LEN, timeout_ms);
    if (n < BM1397_RESPONSE_LEN) {
        return false;
    }

    /* Discriminating a nonce/job response from a register-read response
     * is one of the least-certain parts of this driver - see the
     * confidence note at the top of bm1397.h. Log raw response bytes
     * during bring-up and adjust this if real traffic doesn't match. */
    bool is_job = (buf[6] & 0x01) == 0;

    if (is_job) {
        out->is_job_response = true;
        out->nonce = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] << 8) | buf[3];
        out->job_id = buf[4] & 0xFC;
    } else {
        out->is_job_response = false;
        out->reg_address = buf[4];
        out->reg_value = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                          ((uint32_t)buf[2] << 8) | buf[3];
    }
    return true;
}
