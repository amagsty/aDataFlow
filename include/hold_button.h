#pragma once
#include <stdint.h>

// Nonblocking debounce; long holds fire once and never produce a short click.
class HoldButton
{
public:
    enum class Event { None, Short, Long };
    explicit HoldButton(uint32_t hold_ms) : hold_ms_(hold_ms) {}
    Event update(bool down, uint32_t now)
    {
        if (down != raw_) { raw_ = down; changed_at_ = now; }
        if (raw_ != stable_ && uint32_t(now - changed_at_) >= 35)
        {
            stable_ = raw_;
            if (stable_) { pressed_at_ = changed_at_; long_sent_ = false; }
            else if (!long_sent_)
                return uint32_t(changed_at_ - pressed_at_) >= hold_ms_ ? Event::Long : Event::Short;
        }
        if (stable_ && raw_ && !long_sent_ && uint32_t(now - pressed_at_) >= hold_ms_)
        { long_sent_ = true; return Event::Long; }
        return Event::None;
    }
private:
    uint32_t hold_ms_, changed_at_ = 0, pressed_at_ = 0;
    bool raw_ = false, stable_ = false, long_sent_ = false;
};
