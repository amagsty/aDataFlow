#include "mon.h"
#include "uart.h"

// raw values
uint16_t mon_adc_light;
movAvg3 mon_avg_batt;

// status values
bool mon_is_charging_idle;
bool mon_is_sd_unplugged;
bool mon_is_power_off;
bool mon_is_5v_in;
bool hold_power_on;
bool batt_low;

// readable values
float mon_battery_votage; // 4.000mv

static void task_mon_mem(void *pvParameters)
{
    for (;;)
    {
        log_i("Heap: %d / %d; PSRAM: %d / %d", ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getFreePsram(), ESP.getPsramSize());
        vTaskDelay(5000);
    }
    vTaskDelete(NULL);
}

static void task_mon(void *pvParameters)
{
    uint32_t last_uart_report = millis();
    for (;;)
    {
        const uint32_t now = millis();
        if (now - last_uart_report >= 5000)
        {
            uart_report_drops();
            last_uart_report = now;
        }
        const uint32_t battery_adc_mv = analogReadMilliVolts(PIN_ADC_BATT);
        mon_avg_batt.push(battery_adc_mv);
        mon_battery_votage = (float)mon_avg_batt.result() / 500;

        mon_adc_light = analogRead(PIN_ADC_LIGHT);
        mon_is_charging_idle = digitalRead(PIN_CHARGE_CHK); // GPIO LOW: charging
        mon_is_sd_unplugged = digitalRead(PIN_SD_CD);
        mon_is_power_off = !digitalRead(PIN_BUTTON_CHK); // GPIO HIGH: power on
        mon_is_5v_in = digitalRead(PIN_5V_CHK);
        if (millis() < 10000)
            log_i("POWER startup: adc_mv=%lu, battery=%.3fV, usb=%d, button_off=%d",
                  (unsigned long)battery_adc_mv, mon_battery_votage,
                  int(mon_is_5v_in), int(mon_is_power_off));
        if (mon_battery_votage < BATT_LOW)
        {
            log_w("BATT LOW: %.3fV, usb=%d, button_off=%d; deep sleep in 2 seconds",
                  mon_battery_votage, int(mon_is_5v_in), int(mon_is_power_off));
            batt_low = true;
            vTaskDelay(2000);

            // todo: batt_low alert
            // deep sleep
            digitalWrite(PIN_POWER_HOLD, LOW);
            esp_deep_sleep_start();
            vTaskDelete(NULL);
        }
        if (mon_is_power_off && !hold_power_on)
        {
            // save settings
            save_bandrate_to_nvs();
            log_w("POWER OFF: button_off=%d, battery=%.3fV, usb=%d",
                  int(mon_is_power_off), mon_battery_votage, int(mon_is_5v_in));
            vTaskDelay(10);

            // power off
            digitalWrite(PIN_POWER_HOLD, LOW);
        }
        vTaskDelay(500);
    }
    vTaskDelete(NULL);
}

void mon_init(void)
{
    // xTaskCreatePinnedToCore(task_mon_mem, "mon low freq", 4096, NULL, 1, NULL, 0);
    mon_avg_batt.push(analogReadMilliVolts(PIN_ADC_BATT));
    mon_avg_batt.push(analogReadMilliVolts(PIN_ADC_BATT));
    xTaskCreatePinnedToCore(task_mon, "mon", 4096, NULL, 3, NULL, 0);
}
