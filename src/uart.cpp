#include "uart.h"
#include "ui.h"
#include "HardwareSerial.h"
#include "driver/uart.h"
#include "freertos/semphr.h"
#include <string.h>

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
static SemaphoreHandle_t uart_line_mutex;

typedef struct
{
    char data[UART_LINE_MAX_LENGTH + 1];
    uint16_t length;
    bool dropping_overlong_line;
    bool skip_lf;
} uart_line_buffer_t;

static uart_line_buffer_t rx_line_buffer = {};
static uart_line_buffer_t tx_line_buffer = {};

static void enqueue_uart_line(uart_line_buffer_t *line, bool is_rx)
{
    if (line->length == 0)
        return;

    uart_data_t q = {};
    memcpy(q.data_char, line->data, line->length);
    q.data_char[line->length] = '\0';
    q.data_len = line->length;
    q.is_rx = is_rx;

    if (xQueueSend(uart_queue, &q, 0) != pdPASS)
    {
        log_w("UART line queue full; line dropped");
    }
    line->length = 0;
}

static void collect_uart_lines(HardwareSerial &serial, uart_line_buffer_t *line, bool is_rx)
{
    while (serial.available() > 0)
    {
        int value = serial.read();
        if (value < 0)
            break;

        char c = static_cast<char>(value);
        bool is_line_end = (c == '\r' || c == '\n');

        if (line->dropping_overlong_line)
        {
            if (is_line_end)
            {
                line->dropping_overlong_line = false;
                line->length = 0;
                line->skip_lf = (c == '\r');
            }
            continue;
        }

        if (line->skip_lf && c == '\n')
        {
            line->skip_lf = false;
            continue;
        }
        line->skip_lf = false;

        if (line->length >= UART_LINE_MAX_LENGTH)
        {
            line->length = 0;
            line->dropping_overlong_line = true;
            log_w("UART line exceeded %u bytes; discarding until newline", (unsigned)UART_LINE_MAX_LENGTH);
            if (is_line_end)
                line->dropping_overlong_line = false;
            continue;
        }

        line->data[line->length++] = c;
        if (is_line_end)
        {
            enqueue_uart_line(line, is_rx);
            line->skip_lf = (c == '\r');
        }
    }
}

void onReceive_rx_cb()
{
    xSemaphoreTake(uart_line_mutex, portMAX_DELAY);
    collect_uart_lines(Serial1, &rx_line_buffer, true);
    xSemaphoreGive(uart_line_mutex);
}

void onReceive_tx_cb()
{
    xSemaphoreTake(uart_line_mutex, portMAX_DELAY);
    collect_uart_lines(Serial2, &tx_line_buffer, false);
    xSemaphoreGive(uart_line_mutex);
}

void uart_change_bandrate(uint32_t bandrate)
{
    if (uart_line_mutex == nullptr)
        return;
    xSemaphoreTake(uart_line_mutex, portMAX_DELAY);
    Serial1.flush();
    delay(UART_WAIT);
    Serial2.flush();
    delay(UART_WAIT);
    Serial1.updateBaudRate(bandrate);
    delay(UART_WAIT);
    Serial2.updateBaudRate(bandrate);
    delay(UART_WAIT);
    uart_flush_input(UART_NUM_1);
    uart_flush_input(UART_NUM_2);
    rx_line_buffer = {};
    tx_line_buffer = {};
    xQueueReset(uart_queue);
    xSemaphoreGive(uart_line_mutex);
    log_i("bandrate updated to: %d", bandrate);
}

void uart_init(void)
{
    // Lines are queued only after CR/LF framing, so fewer entries are needed.
    uart_queue = xQueueCreate(128, sizeof(uart_data_t));
    uart_line_mutex = xSemaphoreCreateMutex();
    if (uart_queue == nullptr || uart_line_mutex == nullptr)
    {
        log_e("failed to allocate UART line queue or mutex");
        return;
    }
    
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
