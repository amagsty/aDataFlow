#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
// core1: UI； core0: others
// task priority: 1-3 normal, 4-6 control, 7-9 system/core

#include "main.h"
#include "pins.h"
#include "bandrate.h"
#include "strips.h"
#include "buttons.h"
#include "encoder.h"
#include "mon.h"
#include "ui.h"
#include "sdcard.h"
#include "uart.h"

bool test_mode;

static void my_gpio_init(void)
{
    pinMode(PIN_SD_CD, INPUT_PULLUP);
    pinMode(PIN_POWER_HOLD, OUTPUT);
    pinMode(PIN_LED_REC, OUTPUT);
    pinMode(PIN_CHARGE_CHK, INPUT_PULLUP);
    pinMode(PIN_BUTTON_CHK, INPUT);
    pinMode(PIN_5V_CHK, INPUT);
    digitalWrite(PIN_POWER_HOLD, HIGH);
    digitalWrite(PIN_LED_REC, LOW);
    if (!digitalRead(PIN_BTN_CHART))
        test_mode = true;
}

void setup()
{
    my_gpio_init();
    Serial.begin(115200);
    Serial.printf("[BOOT] aDataFlow %s, reset=%d, internal_heap=%lu, psram=%lu\n",
                  FIRMWARE_VERSION, int(esp_reset_reason()),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned long)ESP.getFreePsram());

    encoders_init();

    // Lights up
    bandrate_init();
    strips_init();

    // Inputs
    buttons_init();
    mon_init();

    // UART
    sdcard_init();
    uart_init();

    // Display
    ui_init();
}

void loop()
{
}
