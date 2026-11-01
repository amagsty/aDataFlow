#include <cassert>
#include <atomic>
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
int log_calls = 0;
#define log_w(...) (++log_calls)
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

static int64_t esp_timer_get_time() { return 123456789; }
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
        assert(record.received_us == 123456789);
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
    // Clear earlier counters, then force both callback failure paths repeatedly.
    take_uart_drop_stats();
    log_calls = 0;
    accept_queue = false;
    for (int n = 0; n < 1000; ++n)
    {
        feed_chunks(port, rx, "drop\n", true);
        feed_chunks(port, tx, "drop\n", false);
    }
    accept_queue = true;
    feed_chunks(port, rx, std::string(UART_LINE_MAX_LENGTH + 1, 'x') + "\n", true);
    feed_chunks(port, tx, std::string(UART_LINE_MAX_LENGTH + 1, 'x') + "\n", false);
    assert(log_calls == 0); // No printf/logging on the UART event-task stack.
    const auto stats = take_uart_drop_stats();
    assert(stats.rx_queue_full == 1000 && stats.tx_queue_full == 1000);
    assert(stats.rx_overlong == 1 && stats.tx_overlong == 1);
    uart_report_drops();
    assert(log_calls == 0); // Counters drained exactly once.
    accept_queue = false;
    feed_chunks(port, rx, "drop\n", true);
    accept_queue = true;
    assert(log_calls == 0);
    uart_report_drops(); // Only the ordinary task reports accumulated failures.
    assert(log_calls == 1);
    uart_report_drops();
    assert(log_calls == 1);
    std::cout << "PASS: actual UART framing, exact 2048-byte payloads with CR/LF/CRLF, overflow recovery, RX/TX separation, full queue and auto-baud discard\n";
}
