#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

using QueueHandle_t = void *;
#include "uart_types.inc"

constexpr int pdPASS = 1;
QueueHandle_t uart_queue = nullptr;
enum class AutoBaudState { Idle, Measuring };
AutoBaudState bandrate_auto_state = AutoBaudState::Idle;
#define log_w(...) ((void)0)
std::vector<uart_data_t> queued;
bool accept_queue = true;
int xQueueSend(QueueHandle_t, const void *item, int wait)
{
    assert(wait == 0);
    if (!accept_queue) return 0;
    queued.push_back(*static_cast<const uart_data_t *>(item));
    return pdPASS;
}
struct HardwareSerial {
    std::deque<unsigned char> bytes;
    size_t available() { return bytes.size(); }
    int read() { if (bytes.empty()) return -1; int c = bytes.front(); bytes.pop_front(); return c; }
    void feed(const std::string &data) { bytes.insert(bytes.end(), data.begin(), data.end()); }
};

#include "uart_functions.inc"

void feed_chunks(HardwareSerial &port, uart_line_buffer_t &line, const std::string &data, bool rx)
{
    size_t offset = 0;
    while (offset < data.size())
    {
        const size_t size = std::min(size_t(17), data.size() - offset);
        port.feed(data.substr(offset, size));
        collect_uart_lines(port, &line, rx);
        offset += size;
    }
}

int main()
{
    HardwareSerial port;
    for (const std::string ending : {"\r", "\n", "\r\n"})
    {
        uart_line_buffer_t line{};
        queued.clear();
        const std::string payload(UART_LINE_MAX_LENGTH, 'x');
        feed_chunks(port, line, payload + ending, true);
        assert(queued.size() == 1);
        const auto &record = queued[0];
        assert(record.data_len == UART_LINE_MAX_LENGTH + 1);
        assert(record.data_char[record.data_len] == '\0');
        assert(std::string(record.data_char, UART_LINE_MAX_LENGTH) == payload);
        assert(record.data_char[UART_LINE_MAX_LENGTH] == ending[0]);
    }
    uart_line_buffer_t line{};
    queued.clear();
    feed_chunks(port, line, std::string(UART_LINE_MAX_LENGTH + 1, 'x') + "\r\n{\"v\":1}\r\n", true);
    assert(queued.size() == 1 && std::string(queued[0].data_char) == "{\"v\":1}\r");

    queued.clear();
    uart_line_buffer_t rx{}, tx{};
    feed_chunks(port, rx, "half", true);
    feed_chunks(port, tx, "other\n", false);
    feed_chunks(port, rx, "line\n", true);
    assert(queued.size() == 2 && !queued[0].is_rx && queued[1].is_rx);
    assert(std::string(queued[1].data_char) == "halfline\n");

    queued.clear();
    line = {};
    accept_queue = false;
    feed_chunks(port, line, "dropped\n", true);
    accept_queue = true;
    feed_chunks(port, line, "next\n", true);
    assert(queued.size() == 1 && std::string(queued[0].data_char) == "next\n");

    queued.clear();
    line = {};
    feed_chunks(port, line, "partial", true);
    bandrate_auto_state = AutoBaudState::Measuring;
    feed_chunks(port, line, "garbage\n", true);
    bandrate_auto_state = AutoBaudState::Idle;
    feed_chunks(port, line, "valid\n", true);
    assert(queued.size() == 1 && std::string(queued[0].data_char) == "valid\n");
    constexpr size_t capacity = UART_QUEUE_MEMORY_BUDGET / sizeof(uart_data_t);
    static_assert(capacity > 0 && capacity * sizeof(uart_data_t) <= UART_QUEUE_MEMORY_BUDGET);
    std::cout << "PASS: actual UART framing, exact 2048-byte payloads with CR/LF/CRLF, overflow recovery, RX/TX separation, full queue and auto-baud discard\n";
}
