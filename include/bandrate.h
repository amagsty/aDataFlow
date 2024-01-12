#ifndef _BACKLIGHT_H
#define _BACKLIGHT_H

#include <Arduino.h>
#include <ShiftRegister74HC595.h>
#include <Preferences.h>
#include "main.h"
#include "pins.h"
#include "encoder.h"
#include "uart.h"

#define BANDRATE_DEFAULT_ID 7 // 115200
#define BANDRATE_C1 2400
#define BANDRATE_C2 1500000

extern uint32_t bandrate_list[12];
extern uint8_t bandrate_index;
void bandrate_init(void);
void turn_off_backlights(void);
void save_bandrate_to_nvs(void);
#endif