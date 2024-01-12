#ifndef _MON_H
#define _MON_H

#include <Arduino.h>
#include "main.h"
#include "pins.h"
#include "avg.h"
#include "bandrate.h"
#include "strips.h"

#define BATT_LOW 3.1 // shutdown under 3.1V

// adc values
extern uint16_t mon_adc_light;

// status values
extern bool mon_is_charging_idle;
extern bool mon_is_sd_unplugged;
extern bool mon_is_5v_in;
extern bool mon_is_power_off;
extern bool hold_power_on;
extern bool batt_low;

// readable values
extern float mon_battery_votage;

void mon_init(void);

#endif

// 一段时间内的环境光最小值、最大值
// 来判断是否要降低升高背光