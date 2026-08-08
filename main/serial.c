/**
 * serial.c — see header. Standard ESP-IDF UART driver usage; nothing
 * BM1397-specific lives here on purpose.
 */

#include "serial.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "serial";
#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024

bool serial_init(int uart_port, int tx_pin, int rx_pin, int baud_rate)
{
    uart_config_t cfg = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_driver_install(uart_port, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed on port %d", uart_port);
        return false;
    }
    if (uart_param_config(uart_port, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed on port %d", uart_port);
        return false;
    }
    if (uart_set_pin(uart_port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed (tx=%d rx=%d)", tx_pin, rx_pin);
        return false;
    }
    ESP_LOGI(TAG, "UART%d up at %d baud (tx=%d rx=%d)", uart_port, baud_rate, tx_pin, rx_pin);
    return true;
}

bool serial_set_baud(int uart_port, int baud_rate)
{
    return uart_set_baudrate(uart_port, baud_rate) == ESP_OK;
}

int serial_write(int uart_port, const uint8_t *data, size_t len)
{
    return uart_write_bytes(uart_port, (const char *)data, len);
}

int serial_read(int uart_port, uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(uart_port, buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

void serial_flush(int uart_port)
{
    uart_flush(uart_port);
}
