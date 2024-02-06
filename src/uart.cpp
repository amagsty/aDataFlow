#include "uart.h"
#include "ui.h"
#include "HardwareSerial.h"

QueueHandle_t uart_queue;
bool is_pause;

void onReceive_cb(HardwareSerial &selected_serial)
{
    uart_data_t q;
    size_t available = selected_serial.available();
    for (int i = 0; i < available; ++i)
    {
        q.data_char[i] = selected_serial.read();
    }
    q.data_char[available] = 0; // Add the terminating nul char
    q.data_len = available;
    q.is_rx = &selected_serial == &Serial1 ? true : false;
    if (!is_mode_changing)
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
    uart_queue = xQueueCreate(200, sizeof(uart_data_t));
    Serial1.setTimeout(UART_TIMEOUT);
    Serial2.setTimeout(UART_TIMEOUT);
    Serial1.onReceive([]()
                      { onReceive_cb(Serial1); });
    Serial2.onReceive([]()
                      { onReceive_cb(Serial2); });
    Serial1.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_RX, -1, false, UART_TIMEOUT);
    Serial2.begin(bandrate_list[bandrate_index], SERIAL_8N1, PIN_TX, -1, false, UART_TIMEOUT);
}