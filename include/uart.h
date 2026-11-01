#ifndef _UART_H
#define _UART_H

#include <Arduino.h>
#include "main.h"
#include "pins.h"
#include "bandrate.h"
#include "sdcard.h"

#define UART_TIMEOUT 50 // DO NOT SET THIS ABOVE 800 (DAFAULT WATCHDOG: 300/800)
#define UART_WAIT 10 // wait for buffer prepared
#define UART_LINE_MAX_LENGTH 2048 // payload bytes, excluding CR/LF
#define UART_QUEUE_MEMORY_BUDGET (48 * 1024) // keep larger records from exhausting RAM

typedef struct queue_t
{
    char data_char[UART_LINE_MAX_LENGTH + 2]; // payload, one line ending, and NUL
    uint16_t data_len;
    bool is_rx;
    uint64_t received_us; // monotonic time when the complete line was queued
} uart_data_t;

struct uart_drop_stats_t
{
    uint32_t rx_queue_full;
    uint32_t tx_queue_full;
    uint32_t rx_overlong;
    uint32_t tx_overlong;
};

extern QueueHandle_t uart_queue;
// Drain counters and report them from a normal task, never a UART callback.
void uart_report_drops();
void uart_change_bandrate(uint32_t bandrate);
void uart_init(void);

#endif
