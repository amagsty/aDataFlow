#include "buttons.h"

static bool is_chart = 0;

static OneButton hw_btn_mode(PIN_BTN_CHART, true);
static OneButton hw_btn_rec(PIN_BTN_REC, true);

static void task_buttons(void *pvParameters)
{
    for (;;)
    {
        hw_btn_mode.tick();
        hw_btn_rec.tick();
        vTaskDelay(50);
    }
    vTaskDelete(NULL);
}

static void toggle_chart(void)
{
    is_mode_changing = !is_mode_changing;
    log_d("is_mode_changing: %d", is_mode_changing);
    // ui_toggle_chart_terminal();
}

static void toggle_rec(void)
{
    is_rec_status_changing = 1;
    log_d("toggle rec: %d", is_rec_status_changing);
}

void buttons_init(void)
{
    hw_btn_mode.attachClick(toggle_chart);
    hw_btn_rec.attachClick(toggle_rec);
    xTaskCreatePinnedToCore(task_buttons, "task_buttons", 2048, NULL, 1, NULL, 0);
}
