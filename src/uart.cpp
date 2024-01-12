#include "uart.h"

QueueHandle_t uart_queue;
bool is_queue_ok;

static void task_uart(void *pvParameters)
{
    uart_data_t q;
    String input;
    while (1)
    {
        if (Serial1.available() > 0)
        {
            input = Serial1.readStringUntil('\n');
            input.trim();
            q.data_string = input.substring(0, 1024);
            q.is_rx = true;
            if (is_queue_ok)
                // xQueueSend(uart_queue, &q, UART_QUEUE_WAIT_MS/portTICK_PERIOD_MS);
                xQueueSend(uart_queue, &q, 0);
            rec(q.data_string, "RX");
        }
        if (Serial2.available() > 0)
        {
            input = Serial2.readStringUntil('\n');
            input.trim();
            q.data_string = input.substring(0, 128);
            q.is_rx = false;
            if (is_queue_ok)
                // xQueueSend(uart_queue, &q, UART_QUEUE_WAIT_MS/portTICK_PERIOD_MS);
                xQueueSend(uart_queue, &q, 0);
            rec(q.data_string, "TX");
        }
        vTaskDelay(UART_TASKDELAY);
    }
}

void uart_change_bandrate(uint32_t bandrate)
{
    Serial1.flush();
    delay(UART_TASKDELAY);
    Serial2.flush();
    delay(UART_TASKDELAY);
    Serial1.updateBaudRate(bandrate);
    delay(UART_TASKDELAY);
    Serial2.updateBaudRate(bandrate);
    delay(UART_TASKDELAY);
    log_i("bandrate updated to: %d", bandrate);
}

void uart_init(void)
{
    uart_queue = xQueueCreate(80, sizeof(uart_data_t));
    Serial1.setTimeout(UART_TIMEOUT);
    Serial2.setTimeout(UART_TIMEOUT);
    Serial1.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_RX, -1, false, UART_TIMEOUT);
    Serial2.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_TX, -1, false, UART_TIMEOUT);
    xTaskCreatePinnedToCore(task_uart, "task_uart", 32768, NULL, 7, NULL, 0);
    is_queue_ok = true;
}