#include "encoder.h"

AiEsp32RotaryEncoder hw_enc_lcd = AiEsp32RotaryEncoder(PIN_ENC_LCD_A, PIN_ENC_LCD_B, PIN_ENC_LCD_BTN, -1, ROTARY_ENCODER_STEPS);
AiEsp32RotaryEncoder hw_enc_bandrate = AiEsp32RotaryEncoder(PIN_ENC_BANDRATE_A, PIN_ENC_BANDRATE_B, PIN_ENC_BANDRATE_BTN, -1, ROTARY_ENCODER_STEPS);

static void hw_enc_bandrate_onButtonClick()
{
    static unsigned long lastTimePressed1 = 0;
    // ignore multiple press in that time milliseconds
    if (millis() - lastTimePressed1 < 500)
    {
        return;
    }
    lastTimePressed1 = millis();
    log_i("hw_enc_bandrate_onButtonClick %lu milliseconds after restart", millis());
}

static void hw_enc_lcd_onButtonClick()
{
    static unsigned long lastTimePressed2 = 0;
    // ignore multiple press in that time milliseconds
    if (millis() - lastTimePressed2 < 500)
    {
        return;
    }
    lastTimePressed2 = millis();
    log_i("hw_enc_lcd_onButtonClick %lu milliseconds after restart", millis());
}

static void task_encoder__test_loop(void *pvParameters)
{
    for (;;)
    {
        if (hw_enc_bandrate.encoderChanged())
        {
            log_d("hw_enc_bandrate changed to: %d", hw_enc_bandrate.readEncoder());
        }
        if (hw_enc_lcd.encoderChanged())
        {
            log_d("hw_enc_lcd changed to: %d", hw_enc_lcd.readEncoder());
        }
        if (hw_enc_bandrate.isEncoderButtonClicked())
        {
            hw_enc_bandrate_onButtonClick();
        }
        if (hw_enc_lcd.isEncoderButtonClicked())
        {
            hw_enc_lcd_onButtonClick();
        }
        vTaskDelay(100);
    }
    vTaskDelete(NULL);
}

static void IRAM_ATTR readEncoderISR(void)
{
    hw_enc_bandrate.readEncoder_ISR();
    hw_enc_lcd.readEncoder_ISR();
}

void encoders_init(void)
{
    hw_enc_bandrate.setup(readEncoderISR);
    hw_enc_lcd.setup(readEncoderISR);
    hw_enc_bandrate.begin();
    hw_enc_lcd.begin();
    hw_enc_bandrate.setAcceleration(0); // or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration
    hw_enc_lcd.setAcceleration(0);      // or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration
    if (!test_mode)
    {
        hw_enc_bandrate.setBoundaries(1, 11, false); // minValue, maxValue, circleValues true|false (when max go to min and vice versa)
    }
}