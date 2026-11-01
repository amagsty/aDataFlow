#ifndef _STRIPS_H
#define _STRIPS_H
#define FASTLED_INTERNAL

#include <Arduino.h>
#include <FastLED.h>
#include "main.h"
#include "pins.h"
#include "uart.h"

#define NUM_LEDS 11

#define STRIP_FRAME_INTERVAL_MS 40

#define STRIP_BRIGHTNESS_MIN 15
#define STRIP_BRIGHTNESS_MAX 100
#define BRIGHTNESS_TOLERANCE 5

void strips_init(void);
void trigger_star_rx(uint32_t star_color);
void trigger_star_tx(uint32_t star_color);
void turn_off_strips(void);

#endif
