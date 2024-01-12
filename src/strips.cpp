#include "strips.h"
#include "avg.h"

struct trigger_info
{
    unsigned long last_ms;
    unsigned long interval_ms;
    bool is_busy;
    uint8_t color;
};

bool is_run_strip_rx;
bool is_run_strip_tx;

static uint8_t strip_mode;

static CRGB leds_rx[NUM_LEDS];
static CRGB leds_tx[NUM_LEDS];

static uint8_t led_index_rx;
static bool is_fading_in_rx;
static uint16_t fading_in_value_rx;
static uint8_t led_update_interval_rx;

static int8_t led_index_tx;
static bool is_fading_in_tx;
static uint16_t fading_in_value_tx;
static uint8_t led_update_interval_tx;

static struct trigger_info trigger_rx;
static struct trigger_info trigger_tx;

static movAvg3 avg_interval_rx;
static movAvg3 avg_interval_tx;

static uint8_t current_brightness;
static uint8_t target_brightness;

void turn_off_strips(void)
{
    FastLED.clear();
    FastLED.show();
}

static void effect_power_on(void)
{
    FastLED.clear();
    FastLED.show();
}

static void fadeToBlackBy2(CRGB *leds, int16_t num_leds, uint8_t fadeBy)
{
    for (uint16_t i = num_leds + 1; i < NUM_LEDS; ++i)
    {
        leds[i].nscale8(255 - fadeBy);
    }
}

static void task_strips_update_rx(void *pvParameters)
{
    for (;;)
    {
        if (is_run_strip_rx)
        {
            // effect: "Meteor"
            if (is_fading_in_rx)
            {
                EVERY_N_MILLISECONDS(led_update_interval_rx)
                {
                    if (fading_in_value_rx > 5)
                    {
                        leds_rx[0] = CHSV(trigger_rx.color, 255, 255);
                        led_index_rx = 1;
                        is_fading_in_rx = false;
                    }
                    else
                    {
                        leds_rx[0] = CHSV(trigger_rx.color, 255, fading_in_value_rx * fading_in_value_rx * 7);
                    }
                    fading_in_value_rx++;
                    FastLED.show();
                }
            }
            else
            {
                if (led_index_rx < NUM_LEDS)
                {
                    leds_rx[led_index_rx] = CHSV(trigger_rx.color, 255, 255);
                    fadeToBlackBy(leds_rx, led_index_rx, 80);
                    led_index_rx++;
                }
                else
                {
                    fadeToBlackBy(leds_rx, led_index_rx, 80);
                    if (leds_rx[NUM_LEDS - 1] == CRGB(0, 0, 0))
                    {
                        is_run_strip_rx = false;
                        trigger_rx.is_busy = false;
                    }
                }
                FastLED.show();
            }

            // // effect: "Ribbon"
            // for (size_t i = NUM_LEDS; i > 1; i--)
            // {
            //     leds_rx[i - 1] = leds_rx[i - 2];
            // }
            // leds_rx[0] = CHSV(trigger_rx.color, 255, random(256));
            // FastLED.show();
            // is_run_strip_rx = false;
            // trigger_rx.is_busy = false;

            // // effect: "Noise"
            // fadeToBlackBy(leds_rx, NUM_LEDS,255);
            // leds_rx[random(NUM_LEDS-1)] = CHSV(trigger_rx.color, 255, 255);
            // FastLED.show();
            // is_run_strip_rx = false;
            // trigger_rx.is_busy = false;
        }
        vTaskDelay(led_update_interval_rx);
    }
    vTaskDelete(NULL);
}

static void task_strips_update_tx(void *pvParameters)
{
    for (;;)
    {
        if (is_run_strip_tx)
        {
            if (is_fading_in_tx)
            {
                EVERY_N_MILLISECONDS(led_update_interval_tx)
                {
                    if (fading_in_value_tx > 5)
                    {
                        leds_tx[NUM_LEDS - 1] = CHSV(trigger_tx.color, 255, 255);
                        led_index_tx = NUM_LEDS - 2;
                        is_fading_in_tx = false;
                    }
                    else
                    {
                        leds_tx[NUM_LEDS - 1] = CHSV(trigger_tx.color, 255, fading_in_value_tx * fading_in_value_tx * 7);
                    }
                    fading_in_value_tx++;
                    FastLED.show();
                }
            }
            else
            {
                if (led_index_tx >= 0)
                {
                    leds_tx[led_index_tx] = CHSV(trigger_tx.color, 255, 255);
                    fadeToBlackBy2(leds_tx, led_index_tx, 80);
                    led_index_tx--;
                }
                else
                {
                    fadeToBlackBy2(leds_tx, led_index_tx, 80);
                    if (leds_tx[0] == CRGB(0, 0, 0))
                    {
                        is_run_strip_tx = false;
                        trigger_tx.is_busy = false;
                    }
                }
                FastLED.show();
            }
        }
        vTaskDelay(led_update_interval_tx);
    }
    vTaskDelete(NULL);
}

static void task_strips_update_brightness(void *pvParameters)
{
    for (;;)
    {
        target_brightness = map(mon_adc_light, 0, 4096, STRIP_BRIGHTNESS_MIN, STRIP_BRIGHTNESS_MAX);
        if ((target_brightness > current_brightness + BRIGHTNESS_TOLERANCE) || (target_brightness < current_brightness - BRIGHTNESS_TOLERANCE))
        {
            while (1)
            {
                if (!is_run_strip_rx && !is_run_strip_tx)
                {
                    FastLED.setBrightness(target_brightness);
                    current_brightness = target_brightness;
                    log_i("strips brightness set to: %d", target_brightness);
                    break;
                }
                vTaskDelay(10);
            }
        }
        vTaskDelay(500);
    }
    vTaskDelete(NULL);
}

static void task_strips_test_mode(void *pvParameters)
{
    uint8_t rxHue = 0;
    uint8_t txHue = 255;
    for (;;)
    {
        rxHue++;
        txHue--;
        fill_rainbow(leds_rx, NUM_LEDS, rxHue, 7);
        fill_rainbow(leds_tx, NUM_LEDS, txHue, 7);
        FastLED.show();
        vTaskDelay(40);
    }
    vTaskDelete(NULL);
}

void trigger_star_rx(uint32_t star_color)
{
    trigger_rx.interval_ms = millis() - trigger_rx.last_ms;
    trigger_rx.last_ms = millis();

    // 30 fps minimum
    avg_interval_rx.push(trigger_rx.interval_ms / 30);
    int32_t result = avg_interval_rx.result();

    if (result > STRIP_TASK_DELAY_MAX)
    {
        led_update_interval_rx = STRIP_TASK_DELAY_MAX;
    }
    else if (result < STRIP_TASK_DELAY_MIN)
    {
        led_update_interval_rx = STRIP_TASK_DELAY_MIN;
    }
    else
    {
        led_update_interval_rx = result;
    }

    if (trigger_rx.is_busy)
        return;

    trigger_rx.color = star_color >> 24;

    // do not fade in the 1st led if interval was too short to increase the refresh speed of the stars
    fading_in_value_rx = led_update_interval_rx >= FADE_IN_FIRST_DOT_THRESHOLD ? 0 : 255;
    is_fading_in_rx = true;
    is_run_strip_rx = true;
    trigger_rx.is_busy = true;
}

void trigger_star_tx(uint32_t star_color)
{
    trigger_tx.interval_ms = millis() - trigger_tx.last_ms;
    trigger_tx.last_ms = millis();

    // 30 fps minimum
    avg_interval_tx.push(trigger_tx.interval_ms / 30);
    int32_t result = avg_interval_tx.result();

    if (result > STRIP_TASK_DELAY_MAX)
    {
        led_update_interval_tx = STRIP_TASK_DELAY_MAX;
    }
    else if (result < STRIP_TASK_DELAY_MIN)
    {
        led_update_interval_tx = STRIP_TASK_DELAY_MIN;
    }
    else
    {
        led_update_interval_tx = result;
    }

    if (trigger_tx.is_busy)
        return;

    trigger_tx.color = star_color >> 24;

    // do not fade in the 1st led if interval was too short to increase the refresh speed of the stars
    fading_in_value_tx = led_update_interval_tx >= FADE_IN_FIRST_DOT_THRESHOLD ? 0 : 255;
    is_fading_in_tx = true;
    is_run_strip_tx = true;
    trigger_tx.is_busy = true;
}

void strips_init(void)
{
    FastLED.addLeds<WS2812B, PIN_WS2812B_S1>(leds_rx, NUM_LEDS);
    FastLED.addLeds<WS2812B, PIN_WS2812B_S2>(leds_tx, NUM_LEDS);
    FastLED.setBrightness(20);
    FastLED.setCorrection(TypicalLEDStrip);
    if (test_mode)
    {
        xTaskCreatePinnedToCore(task_strips_test_mode, "strips_update test mode", 8096, NULL, 1, NULL, 0);
    }
    else
    {
        effect_power_on();
        led_update_interval_rx = STRIP_TASK_DELAY_MAX;
        led_update_interval_tx = STRIP_TASK_DELAY_MAX;
        is_run_strip_rx = false;
        is_run_strip_tx = false;
        xTaskCreatePinnedToCore(task_strips_update_rx, "strips_update rx", 8096, NULL, 8, NULL, 0);
        xTaskCreatePinnedToCore(task_strips_update_tx, "strips_update tx", 8096, NULL, 7, NULL, 0);
        xTaskCreatePinnedToCore(task_strips_update_brightness, "strips_update brightness", 8096, NULL, 7, NULL, 0);
    }
}