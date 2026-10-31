#include "strips.h"
#include <atomic>

namespace {
constexpr uint32_t FRAME_MS = STRIP_FRAME_INTERVAL_MS;
constexpr uint8_t FADE_FRAMES = 6;
constexpr uint8_t TAIL_FRAMES = 20;

enum class EventKind : uint8_t { Rx, Tx, Off };
struct StripEvent {
    EventKind kind;
    uint8_t hue;
};
struct Meteor {
    bool active = false;
    uint8_t hue = 0;
    uint8_t fade_frame = 0;
    uint8_t position = 1;
    uint8_t tail_frame = 0;
};

QueueHandle_t event_queue = nullptr;
CRGB leds_rx[NUM_LEDS];
CRGB leds_tx[NUM_LEDS];
std::atomic<uint32_t> show_started_ms{0};
std::atomic<bool> showing{false};
std::atomic<uint32_t> frames{0};
std::atomic<uint32_t> dropped_events{0};
std::atomic<uint32_t> rx_progress{0};
std::atomic<uint32_t> tx_progress{0};

TickType_t frame_ticks(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

void show_frame()
{
    show_started_ms.store(millis());
    showing.store(true);
    FastLED.show();
    frames.fetch_add(1);
    showing.store(false);
}

void post_event(EventKind kind, uint8_t hue = 0)
{
    if (event_queue == nullptr || test_mode)
        return;
    const StripEvent event{kind, hue};
    // Called from the UI task: LED output must never block data processing.
    if (xQueueSend(event_queue, &event, 0) != pdTRUE)
        dropped_events.fetch_add(1);
}

void start_meteor(Meteor &meteor, uint8_t hue)
{
    if (meteor.active)
        return;
    meteor = Meteor{};
    meteor.active = true;
    meteor.hue = hue;
}

void step_meteor(Meteor &meteor, CRGB *leds, bool reverse)
{
    if (!meteor.active)
        return;

    fadeToBlackBy(leds, NUM_LEDS, 80);
    if (meteor.fade_frame < FADE_FRAMES)
    {
        const uint8_t frame = ++meteor.fade_frame;
        leds[reverse ? NUM_LEDS - 1 : 0] = CHSV(meteor.hue, 255, frame * frame * 7);
    }
    else if (meteor.position < NUM_LEDS)
    {
        const uint8_t index = reverse ? NUM_LEDS - 1 - meteor.position : meteor.position;
        leds[index] = CHSV(meteor.hue, 255, 255);
        ++meteor.position;
    }
    else if (++meteor.tail_frame >= TAIL_FRAMES)
    {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        meteor.active = false;
    }
}

void task_strips(void *)
{
    // Register, initialize, and transmit from this one task on core 0.
    // Test mode uses this exact same output path.
    FastLED.addLeds<WS2812B, PIN_WS2812B_S1>(leds_rx, NUM_LEDS);
    FastLED.addLeds<WS2812B, PIN_WS2812B_S2>(leds_tx, NUM_LEDS);
    FastLED.setCorrection(TypicalLEDStrip);
    uint8_t brightness = 20;
    FastLED.setBrightness(brightness);
    FastLED.clear();
    show_frame();

    Meteor rx;
    Meteor tx;
    uint8_t hue = 0;
    uint32_t last_brightness_ms = 0;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;)
    {
        if (test_mode)
        {
            fill_rainbow(leds_rx, NUM_LEDS, ++hue, 7);
            fill_rainbow(leds_tx, NUM_LEDS, 255 - hue, 7);
        }
        else
        {
            StripEvent event;
            // Bound the amount of event work per frame under continuous traffic.
            for (uint8_t i = 0; i < 16 && xQueueReceive(event_queue, &event, 0) == pdTRUE; ++i)
            {
                if (event.kind == EventKind::Off)
                {
                    rx = Meteor{};
                    tx = Meteor{};
                    FastLED.clear();
                }
                else if (event.kind == EventKind::Rx)
                    start_meteor(rx, event.hue);
                else
                    start_meteor(tx, event.hue);
            }
            step_meteor(rx, leds_rx, false);
            step_meteor(tx, leds_tx, true);
            rx_progress.store(rx.active ? rx.fade_frame + rx.position + rx.tail_frame : 0);
            tx_progress.store(tx.active ? tx.fade_frame + tx.position + tx.tail_frame : 0);

            const uint32_t now = millis();
            if (now - last_brightness_ms >= 500 && !rx.active && !tx.active)
            {
                const int target = constrain(map(mon_adc_light, 0, 4096,
                                                 STRIP_BRIGHTNESS_MIN, STRIP_BRIGHTNESS_MAX),
                                             STRIP_BRIGHTNESS_MIN, STRIP_BRIGHTNESS_MAX);
                if (abs(target - brightness) > BRIGHTNESS_TOLERANCE)
                {
                    brightness = target;
                    FastLED.setBrightness(brightness);
                }
                last_brightness_ms = now;
            }
        }
        // Keep servicing the output driver even between UART packets.
        show_frame();
        vTaskDelayUntil(&last_wake, frame_ticks(FRAME_MS));
    }
}

void task_strip_diagnostics(void *)
{
    uint8_t seconds = 0;
    for (;;)
    {
        vTaskDelay(frame_ticks(1000));
        if (showing.load())
        {
            const uint32_t elapsed = millis() - show_started_ms.load();
            if (elapsed > 1000)
                log_e("LED output stalled in FastLED.show(): %lu ms, frames=%lu, dropped=%lu",
                      (unsigned long)elapsed, (unsigned long)frames.load(),
                      (unsigned long)dropped_events.load());
        }
        if (++seconds == 10)
        {
            log_i("LED frames=%lu, RX step=%lu, TX step=%lu, dropped=%lu",
                  (unsigned long)frames.load(), (unsigned long)rx_progress.load(),
                  (unsigned long)tx_progress.load(), (unsigned long)dropped_events.load());
            seconds = 0;
        }
    }
}
} // namespace

void trigger_star_rx(uint32_t color) { post_event(EventKind::Rx, color >> 24); }
void trigger_star_tx(uint32_t color) { post_event(EventKind::Tx, color >> 24); }
void turn_off_strips(void) { post_event(EventKind::Off); }

void strips_init(void)
{
    event_queue = xQueueCreate(16, sizeof(StripEvent));
    if (event_queue == nullptr)
    {
        log_e("failed to create LED event queue");
        return;
    }
    if (xTaskCreatePinnedToCore(task_strips, "strips", 8192, nullptr, 1, nullptr, 0) != pdPASS)
    {
        log_e("failed to create LED output task");
        vQueueDelete(event_queue);
        event_queue = nullptr;
        return;
    }
    if (xTaskCreatePinnedToCore(task_strip_diagnostics, "strip diagnostics", 3072,
                              nullptr, 1, nullptr, 1) != pdPASS)
        log_e("failed to create LED diagnostics task");
}
