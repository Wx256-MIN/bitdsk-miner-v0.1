/**
 * bm1397.h
 *
 * Driver for the BM1397 SHA-256 mining ASIC.
 *
 * CONFIDENCE NOTE, read before trusting any specific byte value in this
 * driver: Bitmain has never published a datasheet for this chip. Every
 * open-source implementation (Bitaxe's ESP-Miner included) is built on
 * community reverse-engineering. The overall PACKET STRUCTURE below
 * (header byte = TYPE|GROUP|COMMAND, CRC5 on command packets, CRC16 on
 * job packets, chip addressing via a broadcast read + interval-based
 * address assignment, PLL-based frequency control, gradual frequency
 * ramping) matches what ESP-Miner's own architecture documentation
 * describes, so that shape is on reasonably solid ground.
 *
 * The exact register ADDRESSES and the bit-level CRC5/CRC16 ALGORITHMS
 * below are this project's own original implementation of the
 * publicly-documented BM13xx protocol family, written without copying
 * ESP-Miner's source. Treat them as "should be very close" rather than
 * "verified byte-for-byte" - before running real jobs, log every packet
 * this driver sends/receives during BM1397_init() and sanity-check that
 * the chip is actually responding (a real chip ID echoed back on
 * discovery is a good sign; silence or garbage means a framing detail
 * is off and needs debugging, most likely in crc5_bm1397() or the
 * register address table). None of this affects the voltage/thermal
 * safety logic in power_management_task.c, which is deliberately kept
 * independent of whether the ASIC link itself is working.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Packet framing ---- */
#define BM1397_TYPE_CMD        0x40
#define BM1397_TYPE_JOB        0x20
#define BM1397_GROUP_ALL       0x10
#define BM1397_GROUP_SINGLE    0x00
#define BM1397_CMD_SETADDRESS  0x00
#define BM1397_CMD_WRITE       0x01
#define BM1397_CMD_READ        0x02
#define BM1397_CMD_INACTIVE    0x03

#define BM1397_PREAMBLE_0      0x55
#define BM1397_PREAMBLE_1      0xAA

/* ---- Register map (see confidence note above) ---- */
#define BM1397_REG_CHIP_ADDRESS      0x00
#define BM1397_REG_HASH_RATE         0x04   /* BM1397-specific per ESP-Miner's docs */
#define BM1397_REG_PLL0_PARAMETER    0x08
#define BM1397_REG_TICKET_MASK       0x14   /* chip-level share/difficulty filter */
#define BM1397_REG_MISC_CONTROL      0x18
#define BM1397_REG_HASH_COUNTING     0x10
#define BM1397_REG_CORE_REG_CONTROL  0x3C
#define BM1397_REG_ERROR_COUNT       0x4C
#define BM1397_REG_CHIP_NAME         0x00   /* read response to a broadcast read */

#define BM1397_EXPECTED_CHIP_ID_HI   0x13
#define BM1397_EXPECTED_CHIP_ID_LO   0x97

/* ---- Job structure sent to the ASIC ----
 * BM1397 receives a precomputed midstate + the tail fields it needs to
 * finish the header (merkle root's last 4 bytes as folded into
 * extranonce2, ntime, nbits/target) rather than the full 80-byte header -
 * see sha256_midstate.h for why.
 */
typedef struct {
    uint8_t  job_id;              /* 0,4,8,...124 - see BM1397_NEXT_JOB_ID */
    uint8_t  midstate[32];
    uint8_t  merkle_root_tail[4]; /* last 4 bytes of the merkle root */
    uint8_t  ntime[4];
    uint8_t  nbits[4];
    uint32_t starting_nonce;
} bm1397_job_t;

typedef struct {
    bool     is_job_response;
    uint8_t  job_id;
    uint32_t nonce;
    uint16_t midstate_index; /* which of the ASIC's internal midstate slots, if used */
    uint16_t reg_address;    /* valid when is_job_response == false */
    uint32_t reg_value;      /* valid when is_job_response == false */
} bm1397_response_t;

/* Advances a BM1397 job_id by the chip's required +4 stride, wrapping at
 * the 7-bit space (0..124, mask 0xFC per ESP-Miner's documented ID scheme). */
#define BM1397_NEXT_JOB_ID(id) ((uint8_t)(((id) + 4) & 0xFC))

bool     bm1397_init(int expected_chip_count, float initial_freq_mhz, uint16_t initial_voltage_mv);
int      bm1397_detect_chips(int expected_chip_count);
bool     bm1397_set_frequency(float target_mhz);
bool     bm1397_send_job(const bm1397_job_t *job);
bool     bm1397_read_response(bm1397_response_t *out, uint32_t timeout_ms);
bool     bm1397_set_difficulty_mask(uint32_t difficulty);
void     bm1397_request_hashrate_registers(void);

/* Original, from-first-principles implementations of the CRC variants the
 * protocol needs. See the confidence note at the top of this file. */
uint8_t  crc5_bm1397(const uint8_t *data, size_t len);
uint16_t crc16_bm1397(const uint8_t *data, size_t len);
