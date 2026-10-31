#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace chart_validation {
// Leave headroom for LVGL's int32 coordinate * chart-height arithmetic.
constexpr int32_t COORD_LIMIT = 1000000;
constexpr uint32_t SCALE_LIMIT = 1000000;

inline bool valid_format(const char *format)
{
    return format != nullptr && std::strlen(format) == 4 &&
           format[0] == '%' && format[1] == '.' &&
           format[2] >= '0' && format[2] <= '3' && format[3] == 'f';
}

inline bool parse_color(const char *text, uint32_t &color)
{
    if (text == nullptr)
        return false;
    if (text[0] == '#')
        ++text;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    const size_t length = std::strlen(text);
    if (length == 0 || length > 6)
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = text[i];
        uint8_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    color = value;
    return true;
}

inline bool scale_value(double value, uint32_t scale, int32_t &coordinate)
{
    if (!std::isfinite(value) || scale == 0 || scale > SCALE_LIMIT)
        return false;
    const double scaled = value * scale;
    if (!std::isfinite(scaled) || scaled < -COORD_LIMIT || scaled > COORD_LIMIT)
        return false;
    coordinate = static_cast<int32_t>(scaled);
    return true;
}

inline bool make_range(double low, double high, uint32_t scale, int32_t &minimum, int32_t &maximum)
{
    if (low > high)
    {
        const double swap = low;
        low = high;
        high = swap;
    }
    int32_t a, b;
    if (!scale_value(low, scale, a) || !scale_value(high, scale, b) || a >= b)
        return false;
    minimum = a;
    maximum = b;
    return true;
}

template <typename T>
bool find_range(const T *points, size_t count, T missing, T &minimum, T &maximum)
{
    bool found = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (points[i] == missing)
            continue;
        if (!found)
            minimum = maximum = points[i];
        else
        {
            if (points[i] < minimum) minimum = points[i];
            if (points[i] > maximum) maximum = points[i];
        }
        found = true;
    }
    return found;
}
} // namespace chart_validation
