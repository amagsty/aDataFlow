
#include "bandrate.h"
#include "autobaud.h"

ShiftRegister74HC595<2> sr(PIN_74HC595_DS, PIN_74HC595_SHCP, PIN_74HC595_STCP);

uint32_t bandrate_list[12] = {0, BANDRATE_C1, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, BANDRATE_C2};
uint8_t bandrate_index;
volatile AutoBaudState bandrate_auto_state = AutoBaudState::Idle;
static uint8_t bandrate_index_in_nvs;

static uint8_t sr1_value[12] = {B00000000, B00000001, B00000010, B00000100, B00001000, B00010000, B00100000, B01000000, B10000000, B00000000, B00000000, B00000000};
static uint8_t sr2_value[12] = {B10000000, B10000000, B10000000, B10000000, B10000000, B10000000, B10000000, B10000000, B10000000, B10000001, B10000010, B10000100};
static uint8_t pin_values[2];
static uint64_t encoder_value;
static uint64_t bandrate_changed_at_ms;
static bool is_bandrate_changed;
Preferences preferences;

static void task_update_bandrate(void *pvParameters)
{
    for (;;)
    {
        if (hw_enc_bandrate.isEncoderButtonClicked())
        {
            is_bandrate_changed = false;
            if (bandrate_auto_state == AutoBaudState::Measuring)
            {
                autobaud_cancel();
                uart_change_bandrate(bandrate_list[bandrate_index]);
                bandrate_auto_state = AutoBaudState::Idle;
            }
            else
            {
                bandrate_auto_state = autobaud_begin() ? AutoBaudState::Measuring : AutoBaudState::Failed;
                log_i("Auto baud detection started; keep the sender transmitting 8N1 data");
            }
        }

        if (hw_enc_bandrate.encoderChanged())
        {
            encoder_value = hw_enc_bandrate.readEncoder();
            if (encoder_value < 1 || encoder_value > 11)
            {
                hw_enc_bandrate.setEncoderValue(bandrate_index);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            if (bandrate_index != encoder_value && bandrate_auto_state != AutoBaudState::Idle)
            {
                autobaud_cancel();
                bandrate_auto_state = AutoBaudState::Idle;
            }
            pin_values[0] = sr1_value[encoder_value];
            pin_values[1] = sr2_value[encoder_value];
            sr.setAll(pin_values);

            if (bandrate_index != encoder_value)
            {
                is_bandrate_changed = true;
                bandrate_changed_at_ms = millis();
                bandrate_index = encoder_value;
                log_d("hw_enc_bandrate changed to: %d", bandrate_index);
            }
        }

        if (bandrate_auto_state == AutoBaudState::Measuring)
        {
            uint8_t detected_index = 0;
            const AutoBaudResult result = autobaud_poll(detected_index);
            if (result == AutoBaudResult::Found)
            {
                uart_change_bandrate(bandrate_list[detected_index]);
                bandrate_index = detected_index;
                hw_enc_bandrate.setEncoderValue(bandrate_index);
                pin_values[0] = sr1_value[bandrate_index];
                pin_values[1] = sr2_value[bandrate_index];
                sr.setAll(pin_values);
                bandrate_auto_state = AutoBaudState::Idle;
                log_i("Auto baud locked: %lu", (unsigned long)bandrate_list[bandrate_index]);
            }
            else if (result == AutoBaudResult::Failed)
            {
                uart_change_bandrate(bandrate_list[bandrate_index]);
                bandrate_auto_state = AutoBaudState::Failed;
            }
        }

        if (is_bandrate_changed)
        {
            if (millis() - bandrate_changed_at_ms > 700)
            {
                // confirm this bandrate
                // block style
                pin_values[0] = sr1_value[0];
                pin_values[1] = sr2_value[0];
                sr.setAll(pin_values);
                vTaskDelay(100);
                pin_values[0] = sr1_value[bandrate_index];
                pin_values[1] = sr2_value[bandrate_index];
                sr.setAll(pin_values);
                vTaskDelay(100);
                pin_values[0] = sr1_value[0];
                pin_values[1] = sr2_value[0];
                sr.setAll(pin_values);
                vTaskDelay(100);
                pin_values[0] = sr1_value[bandrate_index];
                pin_values[1] = sr2_value[bandrate_index];
                sr.setAll(pin_values);

                uart_change_bandrate(bandrate_list[bandrate_index]);
                hw_enc_bandrate.setEncoderValue(bandrate_index);
                is_bandrate_changed = false;
            }
        }
        vTaskDelay(50);
    }
    vTaskDelete(NULL);
}

void bandrate_init(void)
{
    if (test_mode)
    {
        sr.setAllHigh();
    }
    else
    {
        // load previous bandrate from NVS
        preferences.begin("config", false);
        bandrate_index_in_nvs = preferences.getUChar("bandrate_id", BANDRATE_DEFAULT_ID);
        if (bandrate_index_in_nvs < 1 || bandrate_index_in_nvs > 11)
            bandrate_index_in_nvs = BANDRATE_DEFAULT_ID;
        bandrate_index = bandrate_index_in_nvs;
        preferences.end();
        log_i("default bandrate load from NVS: %d", bandrate_index_in_nvs);

        sr.setAllLow(); // set all pins LOW
        pin_values[0] = sr1_value[bandrate_index];
        pin_values[1] = sr2_value[bandrate_index];
        sr.setAll(pin_values);
        hw_enc_bandrate.setEncoderValue(bandrate_index);
        xTaskCreatePinnedToCore(task_update_bandrate, "task_update_bandrate", 4096, NULL, 4, NULL, 0);
    }
}

void turn_off_backlights(void)
{
    sr.setAllLow(); // set all pins LOW
}

void save_bandrate_to_nvs(void)
{
    const uint8_t selected_index = bandrate_index;
    if (selected_index < 1 || selected_index > 11)
    {
        log_e("refusing to save invalid baud rate index");
        return;
    }
    if (selected_index == bandrate_index_in_nvs)
        return;
    if (!preferences.begin("config", false))
    {
        log_e("failed to open baud rate settings");
        return;
    }
    if (preferences.putUChar("bandrate_id", selected_index) == sizeof(selected_index))
    {
        bandrate_index_in_nvs = selected_index;
        log_i("current bandrate saved to NVS");
    }
    else
        log_e("failed to save baud rate index");
    preferences.end();
}
