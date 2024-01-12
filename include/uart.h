#ifndef _UART_H
#define _UART_H

#include <Arduino.h>
#include "main.h"
#include "pins.h"
#include "bandrate.h"
#include "sdcard.h"

#define UART_TIMEOUT 200 // DO NOT SET THIS ABOVE 800 (DAFAULT WATCHDOG: 300/800)
#define UART_QUEUE_WAIT_MS 100
#define UART_TASKDELAY 20 // up to 50 lines/s

typedef struct queue_t
{
    String data_string;
    bool is_rx;
} uart_data_t;

extern QueueHandle_t uart_queue;
extern bool is_queue_ok;
void uart_change_bandrate(uint32_t bandrate);
void uart_init(void);

#endif