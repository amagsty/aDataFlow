#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Timestamp capacity and resets must match the plotted sample buffer.
template <size_t Capacity> class ChartTimeline
{
public:
    void clear() { next_ = count_ = 0; latest_epoch_us = 0; }
    void push(uint64_t received_us, int64_t epoch_us)
    {
        times_[next_] = received_us;
        next_ = (next_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
        latest_epoch_us = epoch_us;
    }
    bool empty() const { return count_ == 0; }
    uint64_t latest() const { return empty() ? 0 : times_[(next_ + Capacity - 1) % Capacity]; }
    uint64_t span_us() const
    {
        if (count_ < 2) return 0;
        const uint64_t first = times_[(next_ + Capacity - count_) % Capacity];
        return latest() >= first ? latest() - first : 0;
    }
    int64_t latest_epoch_us = 0;
private:
    uint64_t times_[Capacity] = {};
    size_t next_ = 0, count_ = 0;
};

inline void format_chart_span(char *output, size_t capacity, uint64_t microseconds)
{
    const double seconds = microseconds / 1000000.0;
    const char *unit = seconds >= 3600 ? "hr" : seconds >= 60 ? "min" : "s";
    const double value = seconds >= 3600 ? seconds / 3600 : seconds >= 60 ? seconds / 60 : seconds;
    char number[32];
    snprintf(number, sizeof(number), "%.1f", value);
    const size_t length = strlen(number);
    if (length >= 2 && number[length - 2] == '.' && number[length - 1] == '0')
        number[length - 2] = '\0';
    snprintf(output, capacity, "-%s%s", number, unit);
}
