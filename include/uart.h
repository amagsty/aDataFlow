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
} uart_data_t;

extern QueueHandle_t uart_queue;
void uart_change_bandrate(uint32_t bandrate);
void uart_init(void);

#endif
