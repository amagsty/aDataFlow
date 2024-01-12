#include "buttons.h"

static bool is_chart = 0;

static OneButton hw_btn_chart(PIN_BTN_CHART, true);
static OneButton hw_btn_rec(PIN_BTN_REC, true);

static void task_buttons(void *pvParameters)
{
    for (;;)
    {
        hw_btn_chart.tick();
        hw_btn_rec.tick();
        vTaskDelay(100);
    }
    vTaskDelete(NULL);
}

static void toggle_chart(void)
{
    toggle_chart_terminal = !toggle_chart_terminal;
    log_d("toggle chart: %d", toggle_chart_terminal);
    // ui_toggle_chart_terminal();
}

static void toggle_rec(void)
{
    is_rec_status_changing = 1;
    log_d("toggle rec: %d", is_rec_status_changing);
}

void buttons_init(void)
{
    hw_btn_chart.attachClick(toggle_chart);
    hw_btn_rec.attachClick(toggle_rec);
    xTaskCreatePinnedToCore(task_buttons, "task_buttons", 2048, NULL, 1, NULL, 0);
}
