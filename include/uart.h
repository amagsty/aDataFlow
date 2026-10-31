#ifndef _UART_H
#define _UART_H

#include <Arduino.h>
#include "main.h"
#include "pins.h"
#include "bandrate.h"
#include "sdcard.h"

#define UART_TIMEOUT 50 // DO NOT SET THIS ABOVE 800 (DAFAULT WATCHDOG: 300/800)
#define UART_WAIT 10 // wait for buffer prepared
#define UART_LINE_MAX_LENGTH 352 // maximum bytes in one CR/LF-terminated line

typedef struct queue_t
{
    char data_char[UART_LINE_MAX_LENGTH + 1];
    uint16_t data_len;
    bool is_rx;
} uart_data_t;

extern QueueHandle_t uart_queue;
void uart_change_bandrate(uint32_t bandrate);
void uart_init(void);

#endif