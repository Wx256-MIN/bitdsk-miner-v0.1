/**
 * serial.h — thin wrapper around ESP-IDF's UART driver, scoped to exactly
 * what the BM1397 driver needs (init at a given baud, blocking write,
 * timeout-bounded read, runtime baud change, flush). Keeping this
 * separate from bm1397.c means the ASIC driver logic doesn't need to
 * know it's specifically running on ESP-IDF, which makes it easier to
 * unit-test the packet/CRC logic on a desktop later if you want to.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool     serial_init(int uart_port, int tx_pin, int rx_pin, int baud_rate);
bool     serial_set_baud(int uart_port, int baud_rate);
int      serial_write(int uart_port, const uint8_t *data, size_t len);
int      serial_read(int uart_port, uint8_t *buf, size_t max_len, uint32_t timeout_ms);
void     serial_flush(int uart_port);
