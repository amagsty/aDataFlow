#ifndef _ENCODER_H
#define _ENCODER_H

#include <Arduino.h>
#include <AiEsp32RotaryEncoder.h>
#include "main.h"
#include "pins.h"
#define ROTARY_ENCODER_STEPS 4

extern AiEsp32RotaryEncoder hw_enc_lcd;
extern AiEsp32RotaryEncoder hw_enc_bandrate;

void encoders_init(void);

#endif