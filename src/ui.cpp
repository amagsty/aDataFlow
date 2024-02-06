#include "ui.h"

// porting and backlight
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[16000];
static lv_color_t buf2[16000];
static lv_indev_t *indev_encoder;
static lv_group_t *group_encoder;
static int32_t encoder_diff;
static lv_indev_state_t encoder_state;
static LGFX display;
static uint32_t pwm_light;
static uint32_t pwm_light_pre;

// ui
static lv_obj_t *ui_scrMain;
static lv_obj_t *ui_panInfo;
static lv_obj_t *ui_imgSplash;
static lv_obj_t *ui_lblBandrate;
static lv_obj_t *ui_lblDebug;
static lv_obj_t *ui_lblCharge;
static lv_obj_t *ui_lblBattIcon;
static lv_obj_t *ui_lblSdcard;
static lv_obj_t *ui_panPower;
static lv_obj_t *ui_lblLineCounter;
static lv_obj_t *ui_lblBatt;
static lv_obj_t *ui_panContainerTxt;
static lv_obj_t *ui_panList;
static lv_obj_t *ui_panShadow;
static lv_obj_t *ui_panContainerChart;
static lv_obj_t *ui_panCharts;
static lv_obj_t *ui_divisionY;
static lv_obj_t *ui_axisY;
static lv_obj_t *ui_cursorV;
static lv_obj_t *ui_panWaiting;
static lv_obj_t *ui_lblWaiting;
static lv_obj_t *ui_lblQString;
static lv_obj_t *ui_panSeries;
static lv_obj_t *ui_group_chart[CHART_COUNT_MAX];
static lv_chart_series_t *ui_group_chart_series[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_pan[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_cap[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_val[CHART_COUNT_MAX];
static bool ui_info_sd_unplugged;
static bool ui_info_show_batt_votage;
static bool ui_info_is_charging_idle;
static uint8_t ui_info_5v_in;
static uint8_t ui_info_charging;
static uint32_t ui_info_bandrate_index;
static float ui_info_batt_voltage;
static float batt_status_bound[8] = {3.98, 3.87, 3.79, 3.74, 3.68, 3.45, 3.1};
static String batt_status_symbol[5] = {LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_2, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_EMPTY};
static uint8_t batt_status;
static uint8_t ui_batt_status;
static bool line_exceeded;
bool is_mode_changing;

// fonts
LV_FONT_DECLARE(ui_font_SFMono_14);
LV_FONT_DECLARE(ui_font_SFMono_23);
LV_FONT_DECLARE(ui_font_SFMono_9);

// mutex
SemaphoreHandle_t lvgl_mutex = xSemaphoreCreateMutex();

// for info bar
static uint32_t battery_votage_pre = 0;
static uint32_t lineCount = 0;

// mainscr common
static bool is_chart = false;
static bool is_reset_chart_color_needed = true;
static bool is_reset_series_format_needed = true;
static bool is_prepare_sername_needed = true;

// txt terminal
static lv_obj_t *currentLine;
static bool is_rx_last = true;
static bool is_rx_current = true;
static bool panList_scroll_to_view = true;
static long terminal_released_millis;

// chart
// for better movments of cursor on chart
// points should be 1/2, 1/4... of the width of chart
struct chart_range
{
    lv_coord_t max = 0;
    lv_coord_t min = 0;
    bool update_now = false;
    bool update_forced = false;
    bool is_initialized = false;
    uint32_t value_scale = CHART_VALUE_SCALE_DEFAULT;
};
struct chart_series
{
    uint8_t count;
    uint32_t bgcolor;
    uint32_t series_count;
    String sername[CHART_COUNT_MAX];
    uint32_t palette[CHART_COUNT_MAX];
    String value_fmt[CHART_COUNT_MAX];
    bool is_visable[CHART_COUNT_MAX];
    bool is_set_by_config;
};
static struct chart_range chart_ranges[CHART_COUNT_MAX];
static struct chart_series chart_series_prop;
static uint16_t chart_point_count = CHART_WIDTH_PIXEL / 2;
static uint32_t chart_palette[CHART_COUNT_MAX] = COLOR_CHART_PALETTE;
static uint16_t chart_ser_index = 0;

// ----------------------------------------
// task and func
// ----------------------------------------
static void ui_update_chart_range(lv_obj_t *chart, struct chart_range *cr, lv_coord_t value)
{
    cr->update_now = false;
    if (cr->max < value)
    {
        cr->max = value;
        cr->update_now = true;
    }
    if (cr->min > value)
    {
        cr->min = value;
        cr->update_now = true;
    }
    if (cr->update_now || cr->update_forced)
    {
        if (cr->is_initialized)
        {
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, cr->min, cr->max);
            log_i("chart range updated to: %ld, %ld", cr->min, cr->max);
        }
        else
        {
            if (value == 0)
            {
                log_i("chart range initialization skipped for zero value");
            }
            else
            {
                cr->max = value;
                cr->min = value;
                cr->is_initialized = true;
                lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, value, value);
                log_i("chart range initialized to: %ld", value);
            }
        }
        cr->update_forced = false;
    }
}

static void ui_update_chart_color()
{
    lv_obj_set_style_bg_color(ui_panCharts, lv_color_hex(chart_series_prop.bgcolor), LV_PART_MAIN | LV_STATE_DEFAULT);
    for (size_t i = 0; i < CHART_COUNT_MAX; i++)
    {
        lv_chart_set_series_color(ui_group_chart[i], ui_group_chart_series[i], lv_color_hex(chart_series_prop.palette[i]));
        lv_obj_set_style_bg_color(ui_group_legend_cap[i], lv_color_hex(chart_series_prop.palette[i]), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    is_reset_chart_color_needed = false;
}

static void ui_update_chart_config(String &str_config)
{
    /**
     * config template:

        {"config":"weather_station", // config JSON must start with {"config"
        "chart_series":[
            {"name":"ser1", // must exist or it will be ignored
            "min":5, // optional, will be set dynamically
            "max":10, // optional, will be set dynamically
            "scale":100, // optional, default: 100
            "fmt":"%.1f", // optional, default: "%.1f
            "color":"0xFF0000" // optional, default: COLOR_CHART_PALETTE
            },
            ...],
        "chart_bg":"0x000000"} // optional, default: "0x000000"

     */

    JsonDocument doc_config;
    DeserializationError error = deserializeJson(doc_config, str_config);
    if (error)
    {
        log_i("doc_config deserializeJson() failed: %s", error.c_str());
        return;
    }

    // chart bg
    if (!doc_config["chart_bg"].isNull())
    {
        chart_series_prop.bgcolor = strtoimax(doc_config["chart_bg"], NULL, 16); // unknown format will return 0(black)
        is_reset_chart_color_needed = true;
        log_i("chart_bg set to: %u", chart_series_prop.bgcolor);
    }

    JsonArray ser = doc_config["chart_series"].to<JsonArray>();
    uint8_t ser_index = 0;
    for (JsonObject s : ser)
    {
        if (ser_index >= CHART_COUNT_MAX)
            break;

        if (!s["name"].isNull())
        {
            // name
            chart_series_prop.sername[ser_index] = s["name"].as<String>();

            // scale
            if (!s["scale"].isNull())
            {
                chart_ranges[ser_index].value_scale = s["scale"].as<uint32_t>();
                log_i("chart_value_scale: %u - %u", ser_index, chart_ranges[ser_index].value_scale);
            }

            // range
            if (!s["min"].isNull() && !s["max"].isNull())
            {
                if (s["min"].as<long>() > s["max"].as<long>())
                {
                    chart_ranges[ser_index].min = chart_ranges[ser_index].value_scale * s["max"].as<float>();
                    chart_ranges[ser_index].max = chart_ranges[ser_index].value_scale * s["min"].as<float>();
                }
                else
                {
                    chart_ranges[ser_index].min = chart_ranges[ser_index].value_scale * s["min"].as<float>();
                    chart_ranges[ser_index].max = chart_ranges[ser_index].value_scale * s["max"].as<float>();
                }
                chart_ranges[ser_index].is_initialized = true;
                lv_chart_set_range(ui_group_chart[ser_index], LV_CHART_AXIS_PRIMARY_Y, chart_ranges[ser_index].min, chart_ranges[ser_index].max);
                log_i("chart_range is set to: min - %d, max - %d", chart_ranges[ser_index].min, chart_ranges[ser_index].max);
            }
            else
            {
                chart_ranges[ser_index].is_initialized = false;
            }

            // format
            if (!s["fmt"].isNull())
            {
                chart_series_prop.value_fmt[ser_index] = s["fmt"].as<String>();
                log_i("chart_value_fmt: %u - %s", ser_index, chart_series_prop.value_fmt[ser_index]);
            }

            // color
            if (!s["color"].isNull())
            {
                chart_series_prop.palette[ser_index] = strtoimax(s["color"], NULL, 16); // unknown format will return 0(black)
                log_i("chart_palette: %u - %u", ser_index, chart_series_prop.palette[ser_index]);
            }
            ser_index++;
        }
    }
    if (ser_index > 0)
    {
        chart_series_prop.is_set_by_config = true;
        chart_series_prop.series_count = ser_index;
        is_prepare_sername_needed = true;
        is_reset_chart_color_needed = true;
        for (size_t i = 0; i < CHART_COUNT_MAX; i++)
        {
            lv_chart_set_all_value(ui_group_chart[i], ui_group_chart_series[i], LV_CHART_POINT_NONE);
            lv_chart_refresh(ui_group_chart[i]);
            chart_ser_index = 0;
        }
        lv_obj_set_x(ui_cursorV, -20);
        ui_update_chart_color();
    }
}

static void ui_task_update_topbar(void *pvParameters)
{
    for (;;)
    {
        if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
        {
            if (ui_info_bandrate_index != bandrate_index)
            {
                lv_label_set_text_fmt(ui_lblBandrate, "%u", bandrate_list[bandrate_index]);
                ui_info_bandrate_index = bandrate_index;
            }

            if (ui_info_sd_unplugged != mon_is_sd_unplugged)
            {
                if (mon_is_sd_unplugged)
                {
                    lv_label_set_text(ui_lblSdcard, "");
                }
                else
                {
                    lv_label_set_text(ui_lblSdcard, LV_SYMBOL_SD_CARD);
                }
                ui_info_sd_unplugged = mon_is_sd_unplugged;
            }

            if (ui_info_5v_in != mon_is_5v_in || ui_info_is_charging_idle != mon_is_charging_idle)
            {
                if (mon_is_5v_in)
                {
                    if (mon_is_charging_idle)
                    {
                        lv_obj_set_style_text_color(ui_lblCharge, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
                        lv_label_set_text(ui_lblCharge, LV_SYMBOL_USB);
                        lv_obj_add_flag(ui_lblBatt, LV_OBJ_FLAG_HIDDEN);
                    }
                    else
                    {
                        lv_obj_set_style_text_color(ui_lblCharge, lv_color_hex(COLOR_INFO_CHARGE), LV_PART_MAIN | LV_STATE_DEFAULT);
                        lv_label_set_text(ui_lblCharge, LV_SYMBOL_CHARGE);
                        lv_label_set_text(ui_lblBatt, "-.--V");
                        lv_obj_clear_flag(ui_lblBatt, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                else
                {
                    lv_label_set_text(ui_lblCharge, "");
                    lv_label_set_text(ui_lblBatt, "-.--V");
                    lv_obj_clear_flag(ui_lblBatt, LV_OBJ_FLAG_HIDDEN);
                }
                ui_info_5v_in = mon_is_5v_in;
                ui_info_is_charging_idle = mon_is_charging_idle;
            }

            // 100%----4.20V 0 full
            // 90%-----4.06V
            // 80%-----3.98V 1
            // 70%-----3.92V
            // 60%-----3.87V 2
            // 50%-----3.82V
            // 40%-----3.79V 3
            // 30%-----3.77V
            // 20%-----3.74V empty 4
            // 10%-----3.68V flash 5
            // 5%------3.45V alarm 6
            // 1%------3.10V halt 7
            // 0%------3.00V
            for (size_t i = 0; i < sizeof(batt_status_bound); i++)
            {
                if (mon_battery_votage > batt_status_bound[i])
                {
                    batt_status = i;
                    break;
                }
            }

            if (ui_batt_status != batt_status)
            {
                if (batt_status <= 4)
                {
                    lv_obj_set_style_text_color(ui_lblBattIcon, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(ui_lblBattIcon, batt_status_symbol[batt_status].c_str());
                }
                else
                {
                    // TODO: more effects should be added
                    lv_obj_set_style_text_color(ui_lblBattIcon, lv_color_hex(COLOR_INFO_BATT_LOW), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(ui_lblBattIcon, LV_SYMBOL_BATTERY_EMPTY);
                }
                ui_batt_status = batt_status;
            }

            if (ui_info_batt_voltage != mon_battery_votage && lv_obj_is_visible(ui_lblBatt))
            {
                lv_label_set_text_fmt(ui_lblBatt, "%.2fV", mon_battery_votage);
                ui_info_batt_voltage = mon_battery_votage;
            }

            xSemaphoreGive(lvgl_mutex);
        }
        vTaskDelay(500);
    }
    vTaskDelete(NULL);
}

static void ui_term_new_line(bool is_rx)
{
    // remove old lines
    if (line_exceeded)
    {
        lv_obj_del(lv_obj_get_child(ui_panList, 0));
    }
    else
    {
        if (lv_obj_get_child_cnt(ui_panList) >= TERM_ROW_MAX_NUM)
        {
            line_exceeded = true;
        }
    }

    // add one new line
    currentLine = lv_label_create(ui_panList);
    lv_obj_set_width(currentLine, lv_pct(100));
    lv_obj_set_height(currentLine, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(currentLine, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(currentLine, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(currentLine, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(currentLine, -1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(currentLine, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    lv_label_set_text(currentLine, "");
    if (is_rx)
    {
        lv_obj_set_style_text_color(currentLine, lv_color_hex(COLOR_TERM_RX_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(currentLine, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_obj_set_style_text_color(currentLine, lv_color_hex(COLOR_TERM_TX_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(currentLine, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // update line number
    lineCount++;
    lv_label_set_text_fmt(ui_lblLineCounter, "%u", lineCount);
}

static void ui_star_shoot(bool is_rx, char *data)
{
    if (is_rx)
    {
        trigger_star_rx((~crc32_le((uint32_t) ~(0xffffffff), (const uint8_t *)data, strlen(data))) ^ 0xffffffff);
    }
    else
    {
        trigger_star_tx((~crc32_le((uint32_t) ~(0xffffffff), (const uint8_t *)data, strlen(data))) ^ 0xffffffff);
    }
}

static void ui_task_update_main_screen(void *pvParameters)
{
    // common
    uart_data_t q;
    bool is_new_data;
    bool is_new_line;
    bool is_end_with_new_line;
    uint16_t pos_start;
    String current_str;
    bool cuurent_is_rx;

    // chart
    lv_point_t p;
    lv_coord_t point_value;
    float raw_value;
    bool ui_panWaiting_is_visible = true;

    for (;;)
    {
        // wait for changing mode
        if (is_mode_changing)
        {
            vTaskDelay(UI_REFRESH_DELAY);
        }
        else
        {
            // check xQueue
            if (pdPASS == xQueueReceive(uart_queue, &q, 0))
            {
                is_new_data = true;
                rec(q.is_rx, q.data_char);

                if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
                {
                    // header and footer of data_char check
                    if (q.data_char[0] == 10 || q.data_char[0] == 13)
                    {
                        log_d("start with r/n");
                        is_new_line = true;
                    }
                    if (q.data_char[q.data_len - 1] == 10 || q.data_char[q.data_len - 1] == 13)
                    {
                        log_d("end with r/n");
                        is_end_with_new_line = true;
                    }
                    else
                    {
                        is_end_with_new_line = false;
                    }

                    // check format: x x x \r \n x x x ...
                    char *pch;
                    pch = strtok(q.data_char, "\r\n");
                    while (pch != NULL)
                    {
                        if (is_new_line)
                        {
                            current_str = pch;

                            // check if chart-config line: start with {"config" or {'config'
                            if (strcmp(current_str.substring(2, 8).c_str(), "config") == 0)
                            {
                                ui_update_chart_config(current_str);
                            }

                            // update ui
                            if (is_chart)
                            {
                                // update line number
                                lineCount++;
                                lv_label_set_text_fmt(ui_lblLineCounter, "%u", lineCount);
                                ui_star_shoot(q.is_rx, pch);

                                JsonDocument doc; // do not reuse this doc, it will cause memory leak
                                DeserializationError error = deserializeJson(doc, pch);

                                // attention: only JsonObject would be accepted
                                // {"a":123} ok
                                // [123,123] skip

                                if (error || !doc.is<JsonObject>())
                                {
                                    lv_label_set_text(ui_lblQString, pch);
                                    if (!ui_panWaiting_is_visible)
                                    {
                                        lv_obj_fade_in(ui_panWaiting, 100, 0);
                                        ui_panWaiting_is_visible = true;
                                    }
                                }
                                else
                                {
                                    if (is_prepare_sername_needed)
                                    {
                                        if (chart_series_prop.is_set_by_config)
                                        {
                                            // chart ser name was set by config json
                                            for (size_t i = 0; i < CHART_COUNT_MAX; i++)
                                            {
                                                if (i >= chart_series_prop.series_count)
                                                {
                                                    // these series will be hidden
                                                    lv_obj_add_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
                                                    log_i("mark hide chart_series_names: %u", i);
                                                }
                                                else
                                                {
                                                    // reset legend
                                                    lv_obj_clear_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
                                                    lv_label_set_text(ui_group_legend_cap[i], chart_series_prop.sername[i].c_str());
                                                    lv_obj_set_width(ui_group_legend_pan[i], lv_pct(100 / chart_series_prop.series_count));

                                                    log_i("reset chart_series_names_by_config: %u/%u - %s", i, chart_series_prop.series_count, chart_series_prop.sername[i]);
                                                }
                                            }
                                        }
                                        else
                                        {
                                            // Chart will be set by the first line of JSON if no config info received
                                            // make sure series count is under CHART_COUNT_MAX
                                            chart_series_prop.series_count = doc.size() >= CHART_COUNT_MAX ? CHART_COUNT_MAX : doc.size();

                                            JsonObject::iterator it = doc.as<JsonObject>().begin();
                                            for (size_t i = 0; i < CHART_COUNT_MAX; i++)
                                            {
                                                if (i >= chart_series_prop.series_count)
                                                {
                                                    // these series will be hidden
                                                    lv_obj_add_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
                                                    log_i("mark hide chart_series_names: %u", i);
                                                }
                                                else
                                                {
                                                    chart_series_prop.sername[i] = it->key().c_str(); // copy the content

                                                    // reset legend
                                                    lv_obj_clear_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
                                                    lv_label_set_text(ui_group_legend_cap[i], chart_series_prop.sername[i].c_str());
                                                    lv_obj_set_width(ui_group_legend_pan[i], lv_pct(100 / chart_series_prop.series_count));

                                                    log_i("reset chart_series_names: %u/%u - %s", i, chart_series_prop.series_count, chart_series_prop.sername[i]);
                                                    it.operator++();
                                                }
                                            }

                                            // reset chart range/scale
                                            // for (size_t i = 0; i < CHART_COUNT_MAX; i++)
                                            // {
                                            //     chart_ranges[i].is_initialized = false;
                                            // }
                                        }

                                        log_i("chart_series_names inited, count:%d", chart_series_prop.series_count);
                                        is_prepare_sername_needed = false;
                                    }

                                    // update chart
                                    for (size_t i = 0; i < chart_series_prop.series_count; i++)
                                    {
                                        raw_value = doc[chart_series_prop.sername[i]].as<float>();
                                        point_value = raw_value * chart_ranges[i].value_scale;
                                        ui_update_chart_range(ui_group_chart[i], &chart_ranges[i], point_value);
                                        lv_chart_set_next_value(ui_group_chart[i], ui_group_chart_series[i], point_value);
                                        lv_label_set_text_fmt(ui_group_legend_val[i], chart_series_prop.value_fmt[i].c_str(), raw_value);
                                    }

                                    // update the indicator line
                                    lv_chart_get_point_pos_by_id(ui_group_chart[0], ui_group_chart_series[0], chart_ser_index, &p);
                                    lv_obj_set_x(ui_cursorV, p.x + 14);
                                    chart_ser_index++;
                                    if (chart_ser_index >= chart_point_count)
                                    {
                                        chart_ser_index = 0;
                                    }
                                    else
                                    {
                                        for (size_t i = 0; i < chart_series_prop.series_count; i++)
                                        {
                                            lv_chart_set_value_by_id(ui_group_chart[i], ui_group_chart_series[i], chart_ser_index, LV_CHART_POINT_NONE);
                                        }
                                    }

                                    // display the legend
                                    if (ui_panWaiting_is_visible)
                                    {
                                        lv_obj_fade_out(ui_panWaiting, 200, 0);
                                        ui_panWaiting_is_visible = false;
                                    }
                                }
                            }
                            else
                            {
                                ui_term_new_line(q.is_rx);
                                ui_star_shoot(q.is_rx, pch);
                                lv_label_set_text(currentLine, pch);
                            }
                        }
                        else
                        {
                            ui_star_shoot(q.is_rx, pch);
                            if (is_chart)
                            {
                                lv_label_set_text(ui_lblQString, pch);
                            }
                            else
                            {
                                if (strlen(current_str.c_str()) < TERM_LINE_MAX_LENGTH)
                                {
                                    current_str += pch;
                                }
                                else
                                {
                                    ui_term_new_line(q.is_rx);
                                    current_str = pch;
                                }
                                lv_label_set_text(currentLine, current_str.c_str());
                            }
                        }

                        pch = strtok(NULL, "\r\n");
                        is_new_line = true;
                    }
                    is_new_line = is_end_with_new_line;
                    xSemaphoreGive(lvgl_mutex);
                }
            }
            else
            {
                if (is_new_data)
                {
                    if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
                    {
                        // scroll to the bottom directly
                        if (panList_scroll_to_view)
                        {
                            log_d("new data scroll to see it");
                            lv_obj_scroll_to_view(currentLine, LV_ANIM_OFF);
                        }
                        is_new_data = false;
                        xSemaphoreGive(lvgl_mutex);
                    }
                }
                vTaskDelay(UI_REFRESH_DELAY);
            }
        }
    }
}

static void ui_term_new_line_test_mode(const char *s)
{
    // remove old lines
    if (line_exceeded)
    {
        lv_obj_del(lv_obj_get_child(ui_panList, 0));
    }
    else
    {
        if (lv_obj_get_child_cnt(ui_panList) >= TERM_ROW_MAX_NUM)
        {
            line_exceeded = true;
        }
    }
    currentLine = lv_label_create(ui_panList);
    lv_obj_set_width(currentLine, lv_pct(100));
    lv_obj_set_height(currentLine, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(currentLine, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(currentLine, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(currentLine, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(currentLine, -1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(currentLine, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    lv_obj_set_style_text_color(currentLine, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(currentLine, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(currentLine, s);
    if (panList_scroll_to_view)
    {
        lv_obj_scroll_to_view(currentLine, LV_ANIM_OFF);
    }
    lineCount++;
    lv_label_set_text_fmt(ui_lblLineCounter, "%u", lineCount);
}

static void ui_task_update_main_screen_test_mode(void *pvParameters)
{
    // common
    uart_data_t q;
    uint64_t last_mon = millis();

    for (;;)
    {
        if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
        {
            // list actions
            // bandrate
            if (hw_enc_bandrate.encoderChanged())
            {
                char s[20];
                sprintf(s, "EncoderR: %ld", hw_enc_bandrate.readEncoder());
                ui_term_new_line_test_mode(s);
            }
            if (hw_enc_lcd.encoderChanged())
            {
                char s[20];
                sprintf(s, "EncoderL: %ld", hw_enc_lcd.readEncoder());
                ui_term_new_line_test_mode(s);
            }
            if (hw_enc_bandrate.isEncoderButtonDown())
            {
                ui_term_new_line_test_mode("EncoderR: button down");
            }
            if (hw_enc_lcd.isEncoderButtonDown())
            {
                ui_term_new_line_test_mode("EncoderL: button down");
            }

            // mon
            if (millis() - last_mon > 2000)
            {
                char s[50];
                sprintf(s, "ADC: L%04u, B%.3fV; 5V/CHG: %d|%d", mon_adc_light, mon_battery_votage, mon_is_5v_in, !mon_is_charging_idle);
                ui_term_new_line_test_mode(s);
                last_mon = millis();
            }

            // uart
            if (xQueueReceive(uart_queue, &q, 0) == pdTRUE)
            {
                ui_term_new_line_test_mode(q.data_char);
            }

            xSemaphoreGive(lvgl_mutex);
        }

        vTaskDelay(50);
    }
    vTaskDelete(NULL);
}

static void ui_task_toggle_chart_terminal(void *pvParameters)
{
    if (test_mode)
    {
        vTaskDelete(NULL);
    }
    else
    {
        for (;;)
        {
            if (is_mode_changing)
            {
                if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
                {

                    // fade backlight method:
                    for (int16_t i = 250; i >= 0; i -= 10)
                    {
                        // fade out in 359ms
                        display.setBrightness(pwm_light_pre * gamma_table[i] / 255);
                        vTaskDelay(10);
                    }

                    if (is_chart)
                    {
                        lv_obj_set_style_opa(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        is_chart = false;
                    }
                    else
                    {
                        is_prepare_sername_needed = true;
                        if (is_reset_chart_color_needed)
                            ui_update_chart_color();
                        lv_obj_set_style_opa(ui_panContainerChart, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                        is_chart = true;
                    }
                    lv_refr_now(NULL);
                    vTaskDelay(72);
                    for (size_t i = 0; i <= 250; i += 10)
                    {
                        // fade out in 359ms
                        display.setBrightness(pwm_light_pre * gamma_table[i] / 255);
                        vTaskDelay(10);
                    }
                    display.setBrightness(pwm_light_pre);

                    xSemaphoreGive(lvgl_mutex);
                }
                is_mode_changing = false;
            }
            vTaskDelay(100);
        }
        vTaskDelete(NULL);
    }
}

// ----------------------------------------
// porting & backlight
// ----------------------------------------
static void ui_hw_encoder_read(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = hw_enc_lcd.encoderChanged();
    data->state = hw_enc_lcd.isEncoderButtonDown() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void ui_event_panList(lv_event_t *e)
{
    lv_event_code_t panList_event_code = lv_event_get_code(e);
    if (panList_event_code == LV_EVENT_RELEASED)
    {
        if (is_chart)
        {
            // reset range
            log_i("reset %d chart_series range", chart_series_prop.series_count);
            for (size_t i = 0; i < chart_series_prop.series_count; i++)
            {
                lv_coord_t *ser_values = lv_chart_get_y_array(ui_group_chart[i], ui_group_chart_series[i]);
                if (ser_values[0] != 2147483647)
                {
                    chart_ranges[i].min = ser_values[0];
                    chart_ranges[i].max = ser_values[0];
                    for (size_t j = 0; j < chart_point_count; j++)
                    {
                        if (ser_values[j] != 2147483647)
                        {
                            chart_ranges[i].min = ser_values[j] < chart_ranges[i].min ? ser_values[j] : chart_ranges[i].min;
                            chart_ranges[i].max = ser_values[j] > chart_ranges[i].max ? ser_values[j] : chart_ranges[i].max;
                        }
                    }
                    lv_chart_set_range(ui_group_chart[i], LV_CHART_AXIS_PRIMARY_Y, chart_ranges[i].min, chart_ranges[i].max);
                    lv_chart_refresh(ui_group_chart[i]);
                    log_i("chart range reset manually to: %ld, %ld", chart_ranges[i].min, chart_ranges[i].max);
                }
            }
        }
        else
        {
            lv_obj_scroll_to_view(currentLine, LV_ANIM_ON);
            lv_obj_set_scrollbar_mode(ui_panList, LV_SCROLLBAR_MODE_OFF);
            panList_scroll_to_view = true;
            terminal_released_millis = millis();
        }
        log_i("panList_event_code: LV_EVENT_RELEASED");
    }
    if (panList_event_code == LV_EVENT_KEY && panList_scroll_to_view)
    {
        if (millis() - terminal_released_millis > 500)
        {
            lv_obj_set_scrollbar_mode(ui_panList, LV_SCROLLBAR_MODE_ON);
            panList_scroll_to_view = false;
            log_i("panList_event_code: LV_EVENT_SCROLL");
        }
    }
}

static void lv_my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    if (display.getStartCount() == 0)
    { // Processing if not yet started
        display.startWrite();
    }
    display.pushImageDMA(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, (lgfx::swap565_t *)&color_p->full);
    lv_disp_flush_ready(disp);
}

static void lv_port_init(void)
{
    display.begin();
    display.setBrightness(0);
    display.setRotation(1); /* Landscape orientation, flipped */

    // lv_port
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, 16000);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    /*Change the following line to your display resolution*/
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = lv_my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the input device driver for LVGL*/
    if (!test_mode)
    {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_ENCODER;
        indev_drv.read_cb = ui_hw_encoder_read;
        indev_encoder = lv_indev_drv_register(&indev_drv);
        group_encoder = lv_group_create();
        lv_indev_set_group(indev_encoder, group_encoder);
    }
}

static void lv_task_timer(void *pvParameters)
{
    for (;;)
    {
        if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
        {
            lv_timer_handler();
            xSemaphoreGive(lvgl_mutex);
        }
        vTaskDelay(UI_REFRESH_DELAY);
    }
    vTaskDelete(NULL);
}

static void hw_task_backlight(void *pvParameters)
{
    for (;;)
    {
        if (is_mode_changing == false)
        {
            pwm_light = map(mon_adc_light, 0, 4096, ENV_LIGHT_PWM_MIN, ENV_LIGHT_PWM_MAX);
            if (pwm_light > pwm_light_pre + BACKLIGHT_PWM_TOLERANCE)
            {
                // up
                for (size_t i = pwm_light_pre + 1; i <= pwm_light; i++)
                {
                    display.setBrightness(i);
                    vTaskDelay(20);
                }
                pwm_light_pre = pwm_light;
                log_i("lcd brightness faded up to: %u", pwm_light);
            }
            else if (pwm_light < pwm_light_pre - BACKLIGHT_PWM_TOLERANCE)
            {
                // down
                for (size_t i = pwm_light_pre - 1; i >= pwm_light; i--)
                {
                    display.setBrightness(i);
                    vTaskDelay(20);
                }
                pwm_light_pre = pwm_light;
                log_i("lcd brightness faded down to: %u", pwm_light);
            }
        }
        vTaskDelay(500);
    }
    vTaskDelete(NULL);
}

// ----------------------------------------
// ui init
// ----------------------------------------
static void ui_draw_main_screen(void)
{
    // assets
    LV_IMG_DECLARE(ui_img_division_y_png); // assets/division_y.png
    LV_IMG_DECLARE(ui_img_axis_y_png);     // assets/axis_y.png
    LV_IMG_DECLARE(ui_img_cursor_png);     // assets/cursor.png
    LV_IMG_DECLARE(ui_img_splash_png_se);  // assets/splash.png

    // draw screen
    ui_scrMain = lv_obj_create(NULL);
    lv_obj_set_style_radius(ui_scrMain, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_scrMain, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(ui_scrMain, LV_SCROLLBAR_MODE_OFF);

    // terminal interface --------------------------------
    ui_panContainerTxt = lv_obj_create(ui_scrMain);
    lv_obj_set_width(ui_panContainerTxt, 320);
    lv_obj_set_height(ui_panContainerTxt, 200);
    lv_obj_set_x(ui_panContainerTxt, 0);
    lv_obj_set_y(ui_panContainerTxt, -13);
    lv_obj_set_align(ui_panContainerTxt, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_radius(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panContainerTxt, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panContainerTxt, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_panContainerTxt, LV_OBJ_FLAG_EVENT_BUBBLE);                                                                                       /// Flags
    lv_obj_clear_flag(ui_panContainerTxt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags

    ui_imgSplash = lv_img_create(ui_panContainerTxt);
    lv_img_set_src(ui_imgSplash, &ui_img_splash_png_se);
    lv_obj_set_x(ui_imgSplash, 0);
    lv_obj_set_y(ui_imgSplash, -13);
    lv_obj_set_align(ui_imgSplash, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_imgSplash, LV_OBJ_FLAG_SCROLLABLE); /// Flags

    ui_panList = lv_obj_create(ui_panContainerTxt);
    lv_obj_set_width(ui_panList, 307);
    lv_obj_set_height(ui_panList, 200);
    lv_obj_set_align(ui_panList, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_flex_flow(ui_panList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_panList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ui_panList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_panList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panList, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panList, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_panList, LV_OBJ_FLAG_SCROLL_WITH_ARROW | LV_OBJ_FLAG_SCROLL_ONE);                                                                                                                                       /// Flags
    lv_obj_clear_flag(ui_panList, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    lv_group_add_obj(group_encoder, ui_panList);
    lv_obj_add_event_cb(ui_panList, ui_event_panList, LV_EVENT_ALL, NULL);
    static lv_style_t style_scrollbar;
    lv_style_init(&style_scrollbar);
    lv_style_set_width(&style_scrollbar, 3);
    lv_style_set_pad_right(&style_scrollbar, 2);
    lv_obj_add_style(ui_panList, &style_scrollbar, LV_PART_SCROLLBAR);

    currentLine = lv_label_create(ui_panList);
    lv_obj_set_width(currentLine, lv_pct(100));
    lv_obj_set_height(currentLine, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(currentLine, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(currentLine, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(currentLine, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(currentLine, -1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(currentLine, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    lv_label_set_text(currentLine, "");
    lv_obj_set_style_text_color(currentLine, lv_color_hex(COLOR_TERM_RX_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);

    // top info bar --------------------------------
    ui_panInfo = lv_obj_create(ui_scrMain);
    lv_obj_set_height(ui_panInfo, 27);
    lv_obj_set_y(ui_panInfo, -27);
    lv_obj_set_width(ui_panInfo, lv_pct(100));
    lv_obj_set_align(ui_panInfo, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_panInfo, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panInfo, lv_color_hex(COLOR_INFO_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui_panInfo, lv_color_hex(COLOR_INFO_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui_panInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_panInfo, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_panInfo, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_x(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panInfo, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panInfo, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBandrate = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblBandrate, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBandrate, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblBandrate, 0);
    lv_obj_set_y(ui_lblBandrate, 0);
    lv_obj_set_align(ui_lblBandrate, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_lblBandrate, "-");
    lv_obj_set_style_text_color(ui_lblBandrate, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBandrate, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBandrate, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblSdcard = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblSdcard, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblSdcard, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblSdcard, 89);
    lv_obj_set_y(ui_lblSdcard, 0);
    lv_obj_set_align(ui_lblSdcard, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_lblSdcard, "");
    lv_obj_set_style_text_color(ui_lblSdcard, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblSdcard, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblSdcard, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_panPower = lv_obj_create(ui_panInfo);
    lv_obj_set_width(ui_panPower, 120);
    lv_obj_set_height(ui_panPower, lv_pct(100));
    lv_obj_set_align(ui_panPower, LV_ALIGN_RIGHT_MID);
    lv_obj_set_flex_flow(ui_panPower, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_panPower, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ui_panPower, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_bg_color(ui_panPower, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblCharge = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblCharge, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblCharge, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblCharge, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblCharge, "");
    lv_obj_set_y(ui_lblCharge, -1);
    lv_obj_set_style_text_color(ui_lblCharge, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblCharge, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblCharge, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblCharge, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBatt = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblBatt, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBatt, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblBatt, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblBatt, "-.--V");
    lv_obj_set_style_text_color(ui_lblBatt, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBatt, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblBatt, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBatt, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBattIcon = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblBattIcon, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBattIcon, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblBattIcon, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblBattIcon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_y(ui_lblBattIcon, 1);
    lv_obj_set_style_text_color(ui_lblBattIcon, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBattIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblBattIcon, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBattIcon, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblLineCounter = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblLineCounter, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblLineCounter, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblLineCounter, LV_ALIGN_CENTER);
    lv_label_set_text(ui_lblLineCounter, "-");
    lv_obj_set_style_text_color(ui_lblLineCounter, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblLineCounter, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblLineCounter, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    // chart interface --------------------------------
    ui_panContainerChart = lv_obj_create(ui_scrMain);
    lv_obj_set_width(ui_panContainerChart, 320);
    lv_obj_set_height(ui_panContainerChart, 213);
    lv_obj_set_align(ui_panContainerChart, LV_ALIGN_BOTTOM_MID);
    lv_obj_clear_flag(ui_panContainerChart, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panContainerChart, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panContainerChart, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_group_add_obj(group_encoder, ui_panContainerChart);

    ui_panCharts = lv_obj_create(ui_panContainerChart);
    lv_obj_set_width(ui_panCharts, 300);
    lv_obj_set_height(ui_panCharts, 150);
    lv_obj_set_x(ui_panCharts, 0);
    lv_obj_set_y(ui_panCharts, 4);
    lv_obj_set_align(ui_panCharts, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_panCharts, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panCharts, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panCharts, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panCharts, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panCharts, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panCharts, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panCharts, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panCharts, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_divisionY = lv_img_create(ui_panCharts);
    lv_img_set_src(ui_divisionY, &ui_img_division_y_png);
    lv_obj_set_width(ui_divisionY, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_divisionY, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_divisionY, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_divisionY, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
    lv_obj_clear_flag(ui_divisionY, LV_OBJ_FLAG_SCROLLABLE); /// Flags

    ui_axisY = lv_img_create(ui_panCharts);
    lv_img_set_src(ui_axisY, &ui_img_axis_y_png);
    lv_obj_set_width(ui_axisY, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_axisY, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_axisY, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_axisY, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
    lv_obj_clear_flag(ui_axisY, LV_OBJ_FLAG_SCROLLABLE); /// Flags

    for (size_t i = 0; i < CHART_COUNT_MAX; i++)
    {
        ui_group_chart[i] = lv_chart_create(ui_panCharts);
        ui_group_chart_series[i] = lv_chart_add_series(ui_group_chart[i], lv_color_hex(chart_palette[i]), LV_CHART_AXIS_PRIMARY_Y);
        lv_obj_set_width(ui_group_chart[i], CHART_WIDTH_PIXEL);
        lv_obj_set_height(ui_group_chart[i], 124);
        lv_obj_set_align(ui_group_chart[i], LV_ALIGN_CENTER);
        lv_chart_set_type(ui_group_chart[i], LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(ui_group_chart[i], chart_point_count);
        lv_chart_set_div_line_count(ui_group_chart[i], 0, 0);
        lv_chart_set_axis_tick(ui_group_chart[i], LV_CHART_AXIS_PRIMARY_X, 10, 5, 0, 0, false, 50);
        lv_chart_set_axis_tick(ui_group_chart[i], LV_CHART_AXIS_PRIMARY_Y, 10, 5, 0, 0, false, 50);
        lv_chart_set_axis_tick(ui_group_chart[i], LV_CHART_AXIS_SECONDARY_Y, 10, 5, 0, 0, false, 25);
        lv_obj_set_style_radius(ui_group_chart[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_group_chart[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_group_chart[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(ui_group_chart[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(ui_group_chart[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(ui_group_chart[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(ui_group_chart[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_line_width(ui_group_chart[i], 2, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_size(ui_group_chart[i], 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_chart_set_update_mode(ui_group_chart[i], LV_CHART_UPDATE_MODE_CIRCULAR);
    }

    ui_cursorV = lv_img_create(ui_panCharts);
    lv_img_set_src(ui_cursorV, &ui_img_cursor_png);
    lv_obj_set_width(ui_cursorV, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_cursorV, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_cursorV, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_cursorV, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
    lv_obj_clear_flag(ui_cursorV, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_x(ui_cursorV, -20);

    ui_panSeries = lv_obj_create(ui_panContainerChart);
    lv_obj_set_width(ui_panSeries, 300);
    lv_obj_set_height(ui_panSeries, 58);
    lv_obj_set_align(ui_panSeries, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_flex_flow(ui_panSeries, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_panSeries, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ui_panSeries, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panSeries, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panSeries, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panSeries, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panSeries, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (size_t i = 0; i < CHART_COUNT_MAX; i++)
    {
        ui_group_legend_pan[i] = lv_obj_create(ui_panSeries);
        lv_obj_set_height(ui_group_legend_pan[i], 58);
        lv_obj_set_align(ui_group_legend_pan[i], LV_ALIGN_CENTER);
        lv_obj_clear_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_SCROLLABLE); /// Flags
        lv_obj_set_style_radius(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_group_legend_pan[i], lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_group_legend_pan[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(ui_group_legend_pan[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        ui_group_legend_cap[i] = lv_label_create(ui_group_legend_pan[i]);
        lv_obj_set_width(ui_group_legend_cap[i], LV_SIZE_CONTENT);
        lv_obj_set_height(ui_group_legend_cap[i], LV_SIZE_CONTENT); /// 1
        lv_obj_set_x(ui_group_legend_cap[i], 0);
        lv_obj_set_y(ui_group_legend_cap[i], 9);
        lv_obj_set_align(ui_group_legend_cap[i], LV_ALIGN_TOP_MID);
        lv_label_set_long_mode(ui_group_legend_cap[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(ui_group_legend_cap[i], "-");
        lv_obj_set_style_text_color(ui_group_legend_cap[i], lv_color_hex(COLOR_CHART_SERIES_NAME_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_group_legend_cap[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_group_legend_cap[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_group_legend_cap[i], &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(ui_group_legend_cap[i], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_group_legend_cap[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(ui_group_legend_cap[i], 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(ui_group_legend_cap[i], 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(ui_group_legend_cap[i], 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(ui_group_legend_cap[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        ui_group_legend_val[i] = lv_label_create(ui_group_legend_pan[i]);
        lv_obj_set_width(ui_group_legend_val[i], lv_pct(100));
        lv_obj_set_height(ui_group_legend_val[i], LV_SIZE_CONTENT); /// 1
        lv_obj_set_y(ui_group_legend_val[i], -2);
        lv_obj_set_align(ui_group_legend_val[i], LV_ALIGN_BOTTOM_MID);
        lv_label_set_long_mode(ui_group_legend_val[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(ui_group_legend_val[i], "...");
        lv_obj_set_style_text_color(ui_group_legend_val[i], lv_color_hex(COLOR_CHART_SERIES_VALUE_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_group_legend_val[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_group_legend_val[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_group_legend_val[i], &ui_font_SFMono_23, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    ui_panWaiting = lv_obj_create(ui_panContainerChart);
    lv_obj_set_width(ui_panWaiting, 300);
    lv_obj_set_height(ui_panWaiting, 56);
    lv_obj_set_align(ui_panWaiting, LV_ALIGN_BOTTOM_MID);
    lv_obj_clear_flag(ui_panWaiting, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panWaiting, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panWaiting, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panWaiting, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblWaiting = lv_label_create(ui_panWaiting);
    lv_obj_set_width(ui_lblWaiting, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblWaiting, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblWaiting, 3);
    lv_obj_set_y(ui_lblWaiting, 9);
    lv_label_set_text(ui_lblWaiting, "WAITING FOR JSON...");
    lv_obj_set_style_text_color(ui_lblWaiting, lv_color_hex(COLOR_CHART_WAITING_TITLE_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblWaiting, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblWaiting, &ui_font_SFMono_9, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblQString = lv_label_create(ui_panWaiting);
    lv_obj_set_width(ui_lblQString, lv_pct(100));
    lv_obj_set_height(ui_lblQString, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblQString, 0);
    lv_obj_set_y(ui_lblQString, -11);
    lv_label_set_text(ui_lblQString, "...");
    lv_obj_set_align(ui_lblQString, LV_ALIGN_BOTTOM_MID);
    lv_label_set_long_mode(ui_lblQString, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(ui_lblQString, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    lv_obj_set_scrollbar_mode(ui_lblQString, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_color(ui_lblQString, lv_color_hex(COLOR_CHART_WAITING_CONTENT_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblQString, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblQString, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_lblQString, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_lblQString, lv_color_hex(COLOR_CHART_WAITING_CONTENT_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_lblQString, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblQString, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblQString, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblQString, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblQString, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    // init default prop
    for (size_t i = 0; i < CHART_COUNT_MAX; i++)
    {
        chart_series_prop.bgcolor = COLOR_CHART_BG;
        chart_series_prop.palette[i] = chart_palette[i];
        chart_series_prop.value_fmt[i] = CHART_VALUE_FMT_DEFAULT;
    }

    ui_update_chart_color();

    lv_disp_load_scr(ui_scrMain);
}

static void ui_draw_main_screen_test_mode(void)
{
    // draw screen
    ui_scrMain = lv_obj_create(NULL);
    lv_obj_set_style_radius(ui_scrMain, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_scrMain, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(ui_scrMain, LV_SCROLLBAR_MODE_OFF);

    // terminal interface --------------------------------
    ui_panContainerTxt = lv_obj_create(ui_scrMain);
    lv_obj_set_width(ui_panContainerTxt, 320);
    lv_obj_set_height(ui_panContainerTxt, 200);
    lv_obj_set_x(ui_panContainerTxt, 0);
    lv_obj_set_y(ui_panContainerTxt, -13);
    lv_obj_set_align(ui_panContainerTxt, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_radius(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panContainerTxt, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panContainerTxt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_panContainerTxt, LV_OBJ_FLAG_EVENT_BUBBLE);                                                                                       /// Flags
    lv_obj_clear_flag(ui_panContainerTxt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags

    ui_panList = lv_obj_create(ui_panContainerTxt);
    lv_obj_set_width(ui_panList, 307);
    lv_obj_set_height(ui_panList, 200);
    lv_obj_set_align(ui_panList, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_flex_flow(ui_panList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_panList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ui_panList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_panList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panList, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panList, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_panList, LV_OBJ_FLAG_SCROLL_WITH_ARROW | LV_OBJ_FLAG_SCROLL_ONE);                                                                                                                                       /// Flags
    lv_obj_clear_flag(ui_panList, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN); /// Flags
    // lv_group_add_obj(group_encoder, ui_panList);
    lv_obj_add_event_cb(ui_panList, ui_event_panList, LV_EVENT_ALL, NULL);
    static lv_style_t style_scrollbar;
    lv_style_init(&style_scrollbar);
    lv_style_set_width(&style_scrollbar, 3);
    lv_style_set_pad_right(&style_scrollbar, 2);
    lv_obj_add_style(ui_panList, &style_scrollbar, LV_PART_SCROLLBAR);

    currentLine = lv_label_create(ui_panList);
    lv_label_set_text(currentLine, "");

    // top info bar --------------------------------
    ui_panInfo = lv_obj_create(ui_scrMain);
    lv_obj_set_height(ui_panInfo, 27);
    lv_obj_set_width(ui_panInfo, lv_pct(100));
    lv_obj_set_align(ui_panInfo, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_panInfo, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_panInfo, lv_color_hex(0xEEEEEE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panInfo, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panInfo, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panInfo, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBandrate = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblBandrate, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBandrate, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblBandrate, 0);
    lv_obj_set_y(ui_lblBandrate, 0);
    lv_obj_set_align(ui_lblBandrate, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_lblBandrate, "-");
    lv_obj_set_style_text_color(ui_lblBandrate, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBandrate, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBandrate, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_lblBandrate, LV_OBJ_FLAG_HIDDEN); /// Flags

    ui_lblDebug = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblDebug, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblDebug, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblDebug, 0);
    lv_obj_set_y(ui_lblDebug, 0);
    lv_obj_set_align(ui_lblDebug, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_lblDebug, "DEBUG");
    lv_obj_set_style_text_color(ui_lblDebug, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblDebug, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblDebug, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblSdcard = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblSdcard, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblSdcard, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_lblSdcard, 89);
    lv_obj_set_y(ui_lblSdcard, 0);
    lv_obj_set_align(ui_lblSdcard, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_lblSdcard, "");
    lv_obj_set_style_text_color(ui_lblSdcard, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblSdcard, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblSdcard, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_panPower = lv_obj_create(ui_panInfo);
    lv_obj_set_width(ui_panPower, 120);
    lv_obj_set_height(ui_panPower, lv_pct(100));
    lv_obj_set_align(ui_panPower, LV_ALIGN_RIGHT_MID);
    lv_obj_set_flex_flow(ui_panPower, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_panPower, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ui_panPower, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_bg_color(ui_panPower, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_panPower, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblCharge = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblCharge, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblCharge, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblCharge, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblCharge, "");
    lv_obj_set_y(ui_lblCharge, -1);
    lv_obj_set_style_text_color(ui_lblCharge, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblCharge, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblCharge, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblCharge, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblCharge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBatt = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblBatt, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBatt, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblBatt, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblBatt, "-.--V");
    lv_obj_set_style_text_color(ui_lblBatt, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBatt, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblBatt, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBatt, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblBatt, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblBattIcon = lv_label_create(ui_panPower);
    lv_obj_set_width(ui_lblBattIcon, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblBattIcon, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblBattIcon, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(ui_lblBattIcon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_y(ui_lblBattIcon, 1);
    lv_obj_set_style_text_color(ui_lblBattIcon, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblBattIcon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_lblBattIcon, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblBattIcon, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_lblBattIcon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_lblLineCounter = lv_label_create(ui_panInfo);
    lv_obj_set_width(ui_lblLineCounter, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_lblLineCounter, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_lblLineCounter, LV_ALIGN_CENTER);
    lv_label_set_text(ui_lblLineCounter, "-");
    lv_obj_set_style_text_color(ui_lblLineCounter, lv_color_hex(COLOR_INFO_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lblLineCounter, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lblLineCounter, &ui_font_SFMono_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_disp_load_scr(ui_scrMain);
}

static void ui_init_elements_ani(void *pvParameters)
{
    lv_obj_set_style_opa(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_anim_t a_infobar;
    lv_anim_init(&a_infobar);
    lv_anim_set_var(&a_infobar, ui_panInfo);
    lv_anim_set_delay(&a_infobar, 359);
    lv_anim_set_time(&a_infobar, 359);
    lv_anim_set_exec_cb(&a_infobar, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a_infobar, lv_anim_path_ease_out);
    lv_anim_set_values(&a_infobar, -27, 0);
    lv_anim_start(&a_infobar);

    vTaskDelay(5000);

    lv_group_set_editing(group_encoder, true);

    vTaskDelete(NULL);
}

static void ui_init_remove_elements_ani(void *pvParameters)
{
    for (;;)
    {
        if (lineCount > 0)
        {
            lv_obj_del(ui_imgSplash);
            vTaskDelay(100);
            vTaskDelete(NULL);
        }
        vTaskDelay(10);
    }
    vTaskDelete(NULL);
}

// ----------------------------------------
// global
// ----------------------------------------

void ui_init(void)
{
    // init vars
    ui_info_sd_unplugged = !mon_is_sd_unplugged;
    ui_info_bandrate_index = 255;
    ui_info_5v_in = !mon_is_5v_in;
    ui_info_is_charging_idle = !mon_is_charging_idle;

    new_gamma_table(LCD_BKL_GAMMA);
    lv_port_init();

    if (test_mode)
    {
        ui_draw_main_screen_test_mode();
        xTaskCreatePinnedToCore(ui_task_update_main_screen_test_mode, "update main screen test mode", 8192, NULL, 5, NULL, 1);
    }
    else
    {
        ui_draw_main_screen();
        xTaskCreatePinnedToCore(ui_init_elements_ani, "first run animations", 8192, NULL, 9, NULL, 1);
        xTaskCreatePinnedToCore(ui_init_remove_elements_ani, "remove init elements", 8192, NULL, 9, NULL, 1);
        xTaskCreatePinnedToCore(ui_task_update_main_screen, "update main screen", 8192, NULL, 5, NULL, 1);
    }

    xTaskCreatePinnedToCore(lv_task_timer, "lv timer", 8192, NULL, 9, NULL, 1);
    xTaskCreatePinnedToCore(hw_task_backlight, "update backlight", 8192, NULL, 9, NULL, 1);
    xTaskCreatePinnedToCore(ui_task_update_topbar, "update top info bar", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(ui_task_toggle_chart_terminal, "toggle chart terminal", 8192, NULL, 1, NULL, 1);
}