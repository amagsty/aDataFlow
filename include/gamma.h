#ifndef _GAMMA_H
#define _GAMMA_H

#include <Arduino.h>
#include "main.h"
#include "pins.h"

#define LCD_BKL_GAMMA 2.8
#define MAX_PROGRESS 256

extern  uint8_t gamma_table[MAX_PROGRESS];
void new_gamma_table(float gamma);

#endif