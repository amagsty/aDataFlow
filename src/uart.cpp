#include "uart.h"
#include "ui.h"
#include "HardwareSerial.h"
#include "driver/uart.h"
#include "freertos/semphr.h"
#include <string.h>
#include <esp_timer.h>
#include <atomic>

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

// UART callbacks run on Arduino's 2 KB event-task stack. Even log_w's
// vsnprintf can overflow that stack: collect counters here, format elsewhere.
static std::atomic<uint32_t> rx_queue_full{0}, tx_queue_full{0};
static std::atomic<uint32_t> rx_overlong{0}, tx_overlong{0};

static uart_drop_stats_t take_uart_drop_stats()
{
    return {rx_queue_full.exchange(0, std::memory_order_relaxed),
            tx_queue_full.exchange(0, std::memory_order_relaxed),
            rx_overlong.exchange(0, std::memory_order_relaxed),
            tx_overlong.exchange(0, std::memory_order_relaxed)};
}

static void enqueue_uart_line(uart_line_buffer_t *line, bool is_rx)
{
    if (line->length == 0)
        return;

    // Both callbacks hold uart_line_mutex. Keep this large copy off Arduino's
    // small UART event-task stack; xQueueSend copies it before returning.
    static uart_data_t q = {};
    memcpy(q.data_char, line->data, line->length);
    q.data_char[line->length] = '\0';
    q.data_len = line->length;
    q.is_rx = is_rx;
    q.received_us = esp_timer_get_time();

    if (xQueueSend(uart_queue, &q, 0) != pdPASS)
    {
        (is_rx ? rx_queue_full : tx_queue_full).fetch_add(1, std::memory_order_relaxed);
    }
    line->length = 0;
}

static void collect_uart_lines(HardwareSerial &serial, uart_line_buffer_t *line, bool is_rx)
{
    if (bandrate_auto_state == AutoBaudState::Measuring)
        *line = {};
    while (serial.available() > 0)
    {
        int value = serial.read();
        if (value < 0)
            break;
        // The current decoder may be at the wrong baud while hardware measures
        // the signal. Do not display or record those bytes as captured data.
        if (bandrate_auto_state == AutoBaudState::Measuring)
            continue;

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

        if (is_line_end)
        {
            // The delimiter is separate from the payload budget. In particular,
            // a full-size payload followed by CRLF must still be accepted.
            line->data[line->length++] = c;
            enqueue_uart_line(line, is_rx);
            line->skip_lf = (c == '\r');
            continue;
        }

        if (line->length >= UART_LINE_MAX_LENGTH)
        {
            line->length = 0;
            line->dropping_overlong_line = true;
            (is_rx ? rx_overlong : tx_overlong).fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        line->data[line->length++] = c;
    }
}

void uart_report_drops()
{
    const auto stats = take_uart_drop_stats();
    if (stats.rx_queue_full || stats.tx_queue_full || stats.rx_overlong || stats.tx_overlong)
        log_w("UART dropped lines: queue full RX=%lu TX=%lu; overlong RX=%lu TX=%lu",
              (unsigned long)stats.rx_queue_full, (unsigned long)stats.tx_queue_full,
              (unsigned long)stats.rx_overlong, (unsigned long)stats.tx_overlong);
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
    // Bound total queue memory even when the maximum payload size increases.
    constexpr size_t queue_entries = UART_QUEUE_MEMORY_BUDGET / sizeof(uart_data_t);
    static_assert(queue_entries > 0, "UART queue memory budget is too small");
    uart_queue = xQueueCreate(queue_entries, sizeof(uart_data_t));
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
