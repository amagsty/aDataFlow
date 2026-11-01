#include "autobaud.h"
#include "bandrate.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/uart_reg.h"
#include <algorithm>

namespace {
constexpr uint32_t WINDOW_MS = 2500;
constexpr uint8_t MAX_WINDOWS = 4;
bool measuring = false;
uint32_t window_started = 0;
uint8_t windows = 0;
uint8_t previous_rx = 0;
uint8_t previous_tx = 0;
constexpr uint16_t EDGE_SAMPLES = 128;
struct SlowCapture {
    explicit SlowCapture(uint8_t gpio) : pin(gpio) {}
    uint8_t pin;
    bool attached = false;
    bool have_edge = false;
    uint32_t last_edge_us = 0;
    uint16_t count = 0;
    uint16_t edges = 0;
    uint16_t too_fast = 0;
    uint16_t widths[EDGE_SAMPLES] = {};
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
SlowCapture slow_rx(PIN_RX);
SlowCapture slow_tx(PIN_TX);
bool use_slow_rx = false;
bool use_slow_tx = false;

void ARDUINO_ISR_ATTR capture_edge(void *arg)
{
    auto &capture = *static_cast<SlowCapture *>(arg);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    portENTER_CRITICAL_ISR(&capture.mux);
    if (capture.have_edge)
    {
        const uint32_t width = now - capture.last_edge_us;
        if (width < 50)
            ++capture.too_fast;
        else if (width <= 5000 && capture.count < EDGE_SAMPLES)
            capture.widths[capture.count++] = width;
    }
    capture.last_edge_us = now;
    capture.have_edge = true;
    // Bound ISR work even if a sender changes to a high baud rate mid-probe.
    if (++capture.edges >= 512 || capture.count >= EDGE_SAMPLES)
        gpio_ll_intr_disable(&GPIO, capture.pin);
    portEXIT_CRITICAL_ISR(&capture.mux);
}

void start_capture(SlowCapture &capture)
{
    portENTER_CRITICAL(&capture.mux);
    capture.have_edge = false;
    capture.count = capture.edges = capture.too_fast = 0;
    portEXIT_CRITICAL(&capture.mux);
    attachInterruptArg(capture.pin, capture_edge, &capture, CHANGE);
    capture.attached = true;
}

void stop_capture(SlowCapture &capture)
{
    if (capture.attached)
    {
        detachInterrupt(capture.pin);
        capture.attached = false;
    }
}

uint8_t match_slow_rate(SlowCapture &capture)
{
    uint16_t widths[EDGE_SAMPLES];
    portENTER_CRITICAL(&capture.mux);
    const uint16_t count = capture.count;
    const uint16_t too_fast = capture.too_fast;
    std::copy(capture.widths, capture.widths + count, widths);
    portEXIT_CRITICAL(&capture.mux);
    if (count < 16 || too_fast > count / 4)
        return 0;
    std::sort(widths, widths + count);
    // A low quantile keeps single-bit pulses without trusting one short glitch.
    const uint32_t width_us = widths[count / 10];
    const uint32_t measured = 1000000UL / width_us;
    for (uint8_t i = 1; i <= 3; ++i) // 2400, 4800, 9600 only
    {
        const uint32_t rate = bandrate_list[i];
        const uint32_t error = measured > rate ? measured - rate : rate - measured;
        if (uint64_t(error) * 100 <= uint64_t(rate) * 5)
        {
            log_i("Auto baud GPIO%u: pulse=%lu us, samples=%u, candidate=%lu",
                  capture.pin, (unsigned long)width_us, count, (unsigned long)rate);
            return i;
        }
    }
    return 0;
}

bool saturated(const uart_bitrate_res_t &result)
{
    // ESP32-S3 registers are 12-bit. IDF returns register values plus one,
    // so a saturated 4095 register is reported as 4096, not as an error.
    return result.low_period >= UART_LOWPULSE_MIN_CNT_V ||
           result.high_period >= UART_HIGHPULSE_MIN_CNT_V;
}

bool start_window()
{
    uart_bitrate_detect_config_t config = {};
    config.source_clk = UART_SCLK_DEFAULT;
    config.rx_io_num = PIN_RX;
    if (uart_detect_bitrate_start(UART_NUM_1, &config) != ESP_OK)
        return false;
    config.rx_io_num = PIN_TX;
    if (uart_detect_bitrate_start(UART_NUM_2, &config) != ESP_OK)
    {
        uart_bitrate_res_t ignored = {};
        uart_detect_bitrate_stop(UART_NUM_1, false, &ignored);
        return false;
    }
    window_started = millis();
    if (use_slow_rx)
        start_capture(slow_rx);
    if (use_slow_tx)
        start_capture(slow_tx);
    measuring = true;
    return true;
}

uint8_t match_rate(const uart_bitrate_res_t &result)
{
    if (result.edge_cnt == 0 || result.low_period == 0 || result.high_period == 0)
        return 0;
    if (saturated(result))
    {
        log_w("Auto baud counter saturated: low=%lu high=%lu clock=%lu; using GPIO timing",
              (unsigned long)result.low_period, (unsigned long)result.high_period,
              (unsigned long)result.clk_freq_hz);
        return 0;
    }
    const uint64_t period = uint64_t(result.low_period) + result.high_period;
    const uint32_t measured = uint64_t(result.clk_freq_hz) * 2 / period;
    uint8_t best = 0;
    uint64_t best_error = UINT64_MAX;
    for (uint8_t i = 1; i < 12; ++i)
    {
        const uint32_t rate = bandrate_list[i];
        const uint64_t error = measured > rate ? measured - rate : rate - measured;
        const uint64_t relative_error = error * 10000 / rate;
        if (relative_error < best_error)
        {
            best_error = relative_error;
            best = i;
        }
    }
    log_i("Auto baud measured=%lu, candidate=%lu, error=%lu/10000",
          (unsigned long)measured, (unsigned long)bandrate_list[best],
          (unsigned long)best_error);
    return best_error <= 300 ? best : 0; // Within 3% of a selectable baud rate.
}
}

bool autobaud_begin()
{
    autobaud_cancel();
    windows = previous_rx = previous_tx = 0;
    use_slow_rx = use_slow_tx = false;
    if (!uart_is_driver_installed(UART_NUM_1) || !uart_is_driver_installed(UART_NUM_2))
        return false;
    return start_window();
}

void autobaud_cancel()
{
    stop_capture(slow_rx);
    stop_capture(slow_tx);
    if (!measuring)
        return;
    uart_bitrate_res_t ignored = {};
    uart_detect_bitrate_stop(UART_NUM_1, false, &ignored);
    uart_detect_bitrate_stop(UART_NUM_2, false, &ignored);
    measuring = false;
}

AutoBaudResult autobaud_poll(uint8_t &rate_index)
{
    if (!measuring || millis() - window_started < WINDOW_MS)
        return AutoBaudResult::Waiting;

    stop_capture(slow_rx);
    stop_capture(slow_tx);

    uart_bitrate_res_t rx = {};
    uart_bitrate_res_t tx = {};
    const esp_err_t rx_error = uart_detect_bitrate_stop(UART_NUM_1, false, &rx);
    const esp_err_t tx_error = uart_detect_bitrate_stop(UART_NUM_2, false, &tx);
    measuring = false;
    uint8_t rx_rate = rx_error == ESP_OK ? match_rate(rx) : 0;
    uint8_t tx_rate = tx_error == ESP_OK ? match_rate(tx) : 0;
    if (rx_error == ESP_OK && rx.edge_cnt && saturated(rx))
    {
        if (use_slow_rx)
            rx_rate = match_slow_rate(slow_rx);
        use_slow_rx = true;
    }
    if (tx_error == ESP_OK && tx.edge_cnt && saturated(tx))
    {
        if (use_slow_tx)
            tx_rate = match_slow_rate(slow_tx);
        use_slow_tx = true;
    }
    const bool conflict = rx_rate && tx_rate && rx_rate != tx_rate;
    // Require two consecutive measurements on the same direction to agree.
    if (!conflict && ((rx_rate && rx_rate == previous_rx) ||
                      (tx_rate && tx_rate == previous_tx)))
    {
        rate_index = rx_rate && rx_rate == previous_rx ? rx_rate : tx_rate;
        return AutoBaudResult::Found;
    }
    if (conflict)
        log_w("Auto baud: RX/TX disagree; keeping the current baud rate");
    previous_rx = conflict ? 0 : rx_rate;
    previous_tx = conflict ? 0 : tx_rate;
    if (++windows >= MAX_WINDOWS || !start_window())
    {
        log_w("Auto baud: no consistent baud rate detected within 10 seconds");
        return AutoBaudResult::Failed;
    }
    return AutoBaudResult::Waiting;
}
