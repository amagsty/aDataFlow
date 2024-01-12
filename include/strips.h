#ifndef _STRIPS_H
#define _STRIPS_H
#define FASTLED_INTERNAL

#include <Arduino.h>
#include <FastLED.h>
#include "main.h"
#include "pins.h"
#include "uart.h"

#define NUM_LEDS 11

#define STRIP_TASK_DELAY_MIN 5
#define STRIP_TASK_DELAY_MAX 33

#define STRIP_BRIGHTNESS_MIN 15
#define STRIP_BRIGHTNESS_MAX 100
#define BRIGHTNESS_TOLERANCE 5
// show first dot directly without fading effect while led_update_interval_rx is less than this value
#define FADE_IN_FIRST_DOT_THRESHOLD 12

void strips_init(void);
void trigger_star_rx(uint32_t star_color);
void trigger_star_tx(uint32_t star_color);
void turn_off_strips(void);

#endif