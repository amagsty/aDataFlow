#include "uart.h"
#include "ui.h"
#include "HardwareSerial.h"
#include "driver/uart.h"

// Define UART interrupts
typedef enum
{
    UART_INTR_RXFIFO_FULL = (0x1 << 0),
    UART_INTR_TXFIFO_EMPTY = (0x1 << 1),
    UART_INTR_PARITY_ERR = (0x1 << 2),
    UART_INTR_FRAM_ERR = (0x1 << 3),
    UART_INTR_RXFIFO_OVF = (0x1 << 4),
    UART_INTR_DSR_CHG = (0x1 << 5),
    UART_INTR_CTS_CHG = (0x1 << 6),
    UART_INTR_BRK_DET = (0x1 << 7),
    UART_INTR_RXFIFO_TOUT = (0x1 << 8),
    UART_INTR_SW_XON = (0x1 << 9),
    UART_INTR_SW_XOFF = (0x1 << 10),
    UART_INTR_GLITCH_DET = (0x1 << 11),
    UART_INTR_TX_BRK_DONE = (0x1 << 12),
    UART_INTR_TX_BRK_IDLE = (0x1 << 13),
    UART_INTR_TX_DONE = (0x1 << 14),
    UART_INTR_RS485_PARITY_ERR = (0x1 << 15),
    UART_INTR_RS485_FRM_ERR = (0x1 << 16),
    UART_INTR_RS485_CLASH = (0x1 << 17),
    UART_INTR_CMD_CHAR_DET = (0x1 << 18),
    UART_INTR_WAKEUP = (0x1 << 19),
} uart_intr_t;

QueueHandle_t uart_queue;

void onReceive_rx_cb()
{
    uart_data_t q;
    size_t available = Serial1.available();
    Serial1.readBytes(q.data_char, available);
    q.data_char[available] = 0; // Add the terminating nul char
    q.data_len = available;
    q.is_rx = true;
    xQueueSend(uart_queue, (void *)&q, 0); // do not block this task
}
void onReceive_tx_cb()
{
    uart_data_t q;
    size_t available = Serial2.available();
    Serial2.readBytes(q.data_char, available);
    q.data_char[available] = 0; // Add the terminating nul char
    q.data_len = available;
    q.is_rx = false;
    xQueueSend(uart_queue, (void *)&q, 0); // do not block this task
}

void uart_change_bandrate(uint32_t bandrate)
{
    Serial1.flush();
    delay(UART_WAIT);
    Serial2.flush();
    delay(UART_WAIT);
    Serial1.updateBaudRate(bandrate);
    delay(UART_WAIT);
    Serial2.updateBaudRate(bandrate);
    delay(UART_WAIT);
    log_i("bandrate updated to: %d", bandrate);
}

void uart_init(void)
{
    uart_queue = xQueueCreate(500, sizeof(uart_data_t));
    
    Serial1.setTimeout(UART_TIMEOUT);
    Serial2.setTimeout(UART_TIMEOUT);

    // override default RX IDF Driver Interrupt - no BREAK, PARITY or OVERFLOW
    uart_intr_config_t uart_intr = {
        .intr_enable_mask = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT, // only these IRQs - no BREAK, PARITY or OVERFLOW
        .rx_timeout_thresh = 1,
        .txfifo_empty_intr_thresh = 10,
        .rxfifo_full_thresh = 2,
    };
    uart_intr_config(UART_NUM_1, &uart_intr);
    uart_intr_config(UART_NUM_2, &uart_intr);

    Serial1.onReceive([]()
                      { onReceive_rx_cb(); });
    Serial2.onReceive([]()
                      { onReceive_tx_cb(); });

    Serial1.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_RX, -1, false, UART_TIMEOUT);
    Serial2.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_TX, -1, false, UART_TIMEOUT);
}