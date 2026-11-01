#include "ui.h"
#include "chart_validation.h"
#include <stdio.h>
#include <esp_heap_caps.h>

// porting and backlight
static lv_disp_draw_buf_t draw_buf;
// Two 16-line DMA tiles leave internal RAM for Wi-Fi and UART task stacks.
static constexpr size_t DRAW_BUFFER_PIXELS = SCREEN_WIDTH * 16;
static lv_color_t buf1[DRAW_BUFFER_PIXELS];
static lv_color_t buf2[DRAW_BUFFER_PIXELS];
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
bool is_mode_changing;

// fonts
LV_FONT_DECLARE(ui_font_SFMono_14);
LV_FONT_DECLARE(ui_font_SFMono_23);
LV_FONT_DECLARE(ui_font_SFMono_9);

// mutex
SemaphoreHandle_t lvgl_mutex = nullptr;

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

// ----------------------------------------
// task and func
// ----------------------------------------
static char ui_legend_value_text[CHART_COUNT_MAX][48];

static const lv_font_t *ui_fit_legend_font(bool captions)
{
    const lv_font_t *fonts[] = {&ui_font_SFMono_23, &ui_font_SFMono_14, &ui_font_SFMono_9};
    const size_t first = captions || chart_series_prop.series_count >= 4 ? 1 : 0;
    for (size_t candidate = first; candidate < 3; ++candidate)
    {
        bool fits = true;
        for (size_t i = 0; i < chart_series_prop.series_count; ++i)
        {
            lv_obj_t *label = captions ? ui_group_legend_cap[i] : ui_group_legend_val[i];
            const char *text = captions ? chart_series_prop.sername[i].c_str() : ui_legend_value_text[i];
            const lv_coord_t spacing = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
            if (lv_txt_get_width(text, strlen(text), fonts[candidate], spacing, LV_TEXT_FLAG_NONE) >
                lv_obj_get_content_width(label))
            {
                fits = false;
                break;
            }
        }
        if (fits)
            return fonts[candidate];
    }
    return &ui_font_SFMono_9;
}

static void ui_refresh_legend_values()
{
    // Use one font for the entire value row, based on its widest value.
    const lv_font_t *font = ui_fit_legend_font(false);
    for (size_t i = 0; i < chart_series_prop.series_count; ++i)
    {
        lv_obj_t *label = ui_group_legend_val[i];
        if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font)
            lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
        if (strcmp(lv_label_get_text(label), ui_legend_value_text[i]) != 0)
            lv_label_set_text(label, ui_legend_value_text[i]);
    }
}

static void ui_set_legend_text(size_t index, const char *text)
{
    snprintf(ui_legend_value_text[index], sizeof(ui_legend_value_text[index]), "%s", text);
    ui_refresh_legend_values();
}

static void ui_set_legend_number(size_t index, double value)
{
    char text[48];
    snprintf(text, sizeof(text), chart_series_prop.value_fmt[index].c_str(), value);
    lv_obj_t *label = ui_group_legend_val[index];
    const lv_coord_t spacing = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    if (lv_txt_get_width(text, strlen(text), &ui_font_SFMono_9, spacing, LV_TEXT_FLAG_NONE) >
        lv_obj_get_content_width(label))
        snprintf(text, sizeof(text), "%.2e", value);
    ui_set_legend_text(index, text);
}

static void ui_update_legend_layout()
{
    for (size_t i = 0; i < CHART_COUNT_MAX; ++i)
    {
        if (i >= chart_series_prop.series_count)
            lv_obj_add_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
        else
        {
            lv_obj_clear_flag(ui_group_legend_pan[i], LV_OBJ_FLAG_HIDDEN);
            strcpy(ui_legend_value_text[i], "--");
        }
    }
    lv_obj_update_layout(ui_panSeries);
    // Captions share one font as well, selected against every visible name.
    const lv_font_t *font = ui_fit_legend_font(true);
    for (size_t i = 0; i < chart_series_prop.series_count; ++i)
    {
        lv_obj_set_style_text_font(ui_group_legend_cap[i], font, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(ui_group_legend_cap[i], chart_series_prop.sername[i].c_str());
    }
    ui_refresh_legend_values();
}

static void ui_update_chart_range(lv_obj_t *chart, struct chart_range *cr, lv_coord_t value)
{
    cr->update_now = false;
    if (!cr->is_initialized)
    {
        cr->min = cr->max = value;
        cr->is_initialized = true;
        cr->update_now = true;
    }
    else
    {
        if (value < cr->min) { cr->min = value; cr->update_now = true; }
        if (value > cr->max) { cr->max = value; cr->update_now = true; }
    }
    if (cr->update_now || cr->update_forced)
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, cr->min, cr->max);
    cr->update_forced = false;
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

static void ui_update_chart_config(JsonDocument &config)
{
    uint32_t color;
    if (!config["chart_bg"].isNull())
    {
        if (chart_validation::parse_color(config["chart_bg"].as<const char *>(), color))
        {
            chart_series_prop.bgcolor = color;
            is_reset_chart_color_needed = true;
        }
        else
            log_w("invalid chart_bg; keeping previous color");
    }

    // Read the existing array without converting/clearing it.
    JsonArrayConst series = config["chart_series"].as<JsonArrayConst>();
    uint8_t index = 0;
    for (JsonObjectConst item : series)
    {
        if (index >= CHART_COUNT_MAX)
            break;
        const char *name = item["name"].as<const char *>();
        if (name == nullptr || name[0] == '\0' || strlen(name) > 64)
        {
            log_w("invalid chart series name; skipping entry");
            continue;
        }
        bool duplicate = false;
        for (uint8_t i = 0; i < index; ++i)
            duplicate |= chart_series_prop.sername[i] == name;
        if (duplicate)
        {
            log_w("duplicate chart series name: %s", name);
            continue;
        }

        chart_series_prop.sername[index] = name;
        chart_ranges[index] = chart_range{};
        chart_series_prop.palette[index] = chart_palette[index];
        chart_series_prop.value_fmt[index] = CHART_VALUE_FMT_DEFAULT;

        if (!item["scale"].isNull())
        {
            if (item["scale"].is<uint32_t>() && item["scale"].as<uint32_t>() > 0 &&
                item["scale"].as<uint32_t>() <= chart_validation::SCALE_LIMIT)
                chart_ranges[index].value_scale = item["scale"].as<uint32_t>();
            else
                log_w("invalid chart scale; using default");
        }

        if (!item["min"].isNull() || !item["max"].isNull())
        {
            int32_t low, high;
            if (item["min"].is<double>() && item["max"].is<double>() &&
                chart_validation::make_range(item["min"].as<double>(), item["max"].as<double>(),
                                             chart_ranges[index].value_scale, low, high))
            {
                chart_ranges[index].min = low;
                chart_ranges[index].max = high;
                chart_ranges[index].is_initialized = true;
                lv_chart_set_range(ui_group_chart[index], LV_CHART_AXIS_PRIMARY_Y, low, high);
            }
            else
                log_w("invalid chart range; using automatic range");
        }

        if (!item["fmt"].isNull())
        {
            const char *format = item["fmt"].as<const char *>();
            if (chart_validation::valid_format(format))
                chart_series_prop.value_fmt[index] = format;
            else
                log_w("invalid chart format; using default");
        }
        if (!item["color"].isNull())
        {
            if (chart_validation::parse_color(item["color"].as<const char *>(), color))
                chart_series_prop.palette[index] = color;
            else
                log_w("invalid chart color; using default");
        }
        ++index;
    }
    if (index > 0)
    {
        chart_series_prop.is_set_by_config = true;
        chart_series_prop.series_count = index;
        is_prepare_sername_needed = true;
        is_reset_chart_color_needed = true;
        for (size_t i = 0; i < CHART_COUNT_MAX; ++i)
            lv_chart_set_all_value(ui_group_chart[i], ui_group_chart_series[i], LV_CHART_POINT_NONE);
        lv_obj_set_x(ui_cursorV, -20);
    }
    if (is_reset_chart_color_needed)
        ui_update_chart_color();
}

static void ui_task_update_topbar(void *pvParameters)
{
    AutoBaudState displayed_auto_state = AutoBaudState::Idle;
    for (;;)
    {
        if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
        {
            const AutoBaudState auto_state = bandrate_auto_state;
            if (ui_info_bandrate_index != bandrate_index || displayed_auto_state != auto_state)
            {
                if (auto_state == AutoBaudState::Measuring)
                    lv_label_set_text(ui_lblBandrate, "AUTO...");
                else if (auto_state == AutoBaudState::Failed)
                    lv_label_set_text(ui_lblBandrate, "AUTO?");
                else
                    lv_label_set_text_fmt(ui_lblBandrate, "%lu", (unsigned long)bandrate_list[bandrate_index]);
                ui_info_bandrate_index = bandrate_index;
                displayed_auto_state = auto_state;
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
    // Keep history bounded even while the terminal is covered by the chart.
    while (lv_obj_get_child_cnt(ui_panList) >= TERM_ROW_MAX_NUM)
        lv_obj_del(lv_obj_get_child(ui_panList, 0));

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
    lv_label_set_text_fmt(ui_lblLineCounter, "%lu", (unsigned long)lineCount);
}

// Keep the original queued bytes separate from display and JSON parsing.
struct terminal_row
{
    size_t length;
    uint16_t hex_columns;
    bool hex;
    char data[1]; // raw bytes + NUL, followed by plain display text
};
static bool terminal_hex = false;
static uint32_t terminal_pressed_ms = 0;
static bool terminal_press_pending = false;
static bool terminal_pressed_in_chart = false;
static constexpr uint32_t TERMINAL_LONG_PRESS_MS = 2000;
static constexpr size_t TERMINAL_HEX_PREVIEW_BYTES = 256;

static void ui_terminal_row_delete(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_target(event);
    free(lv_obj_get_user_data(label));
    lv_obj_set_user_data(label, nullptr);
}

// Draw byte colors directly. The text buffer contains no color commands, so
// wrapping or scrolling can never expose markup or lose its parser state.
static void ui_draw_terminal_hex(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_target(event);
    auto *row = static_cast<terminal_row *>(lv_obj_get_user_data(label));
    if (row == nullptr || !row->hex) return;
    lv_draw_ctx_t *context = lv_event_get_draw_ctx(event);
    lv_draw_label_dsc_t draw;
    lv_draw_label_dsc_init(&draw);
    draw.font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    draw.letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    draw.opa = lv_obj_get_style_opa_recursive(label, LV_PART_MAIN);
    draw.bidi_dir = LV_BASE_DIR_LTR;
    const lv_color_t primary = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    const lv_color_t alternate = primary.full == lv_color_hex(COLOR_TERM_TX_TEXT).full ?
                                lv_color_hex(COLOR_TERM_RX_TEXT) : lv_color_hex(0x6AC5DB);
    const lv_coord_t byte_width = lv_txt_get_width("00", 2, draw.font, draw.letter_space, LV_TEXT_FLAG_NONE);
    const lv_coord_t line_height = lv_font_get_line_height(draw.font);
    const lv_coord_t line_step = line_height + lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    lv_area_t content;
    lv_obj_get_content_coords(label, &content);
    const bool align_right = lv_obj_get_style_text_align(label, LV_PART_MAIN) == LV_TEXT_ALIGN_RIGHT;
    const char *line = row->data + row->length + 1;
    const size_t shown = row->length < TERMINAL_HEX_PREVIEW_BYTES ? row->length : TERMINAL_HEX_PREVIEW_BYTES;
    size_t byte_index = 0;
    lv_coord_t y = content.y1;
    while (*line)
    {
        const char *end = strchr(line, '\n');
        const size_t length = end ? size_t(end - line) : strlen(line);
        const lv_coord_t width = lv_txt_get_width(line, length, draw.font, draw.letter_space, LV_TEXT_FLAG_NONE);
        lv_coord_t x = align_right ? content.x2 - width + 1 : content.x1;
        if (byte_index < shown)
        {
            for (size_t i = 0; i + 1 < length; i += 2, ++byte_index)
            {
                const char byte[] = {line[i], line[i + 1], '\0'};
                draw.color = byte_index % 2 == 0 ? primary : alternate;
                const lv_area_t area = {x, y, lv_coord_t(x + byte_width - 1), lv_coord_t(y + line_height - 1)};
                lv_draw_label(context, &draw, &area, byte, nullptr);
                x += byte_width + draw.letter_space;
            }
        }
        else
        {
            draw.color = primary;
            const lv_area_t area = {x, y, content.x2, lv_coord_t(y + line_height - 1)};
            lv_draw_label(context, &draw, &area, line, nullptr);
        }
        if (end == nullptr) break;
        line = end + 1;
        y += line_step;
    }
}

static void ui_render_terminal_row(lv_obj_t *label)
{
    auto *row = static_cast<terminal_row *>(lv_obj_get_user_data(label));
    if (row == nullptr)
        return;
    char *output = row->data + row->length + 1;
    row->hex = terminal_hex;
    if (terminal_hex)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        size_t pos = 0;
        lv_obj_update_layout(label);
        const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        const lv_coord_t spacing = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
        const lv_coord_t stride = lv_txt_get_width("00", 2, font, spacing, LV_TEXT_FLAG_NONE) + spacing;
        const lv_coord_t width = lv_obj_get_content_width(label);
        const int columns = stride > 0 ? (width + spacing) / stride : 1;
        row->hex_columns = columns > 0 ? columns : 1;
        // Fifty fully wrapped 2KB HEX rows exceed LVGL's 16-bit scroll range.
        const size_t shown = row->length < TERMINAL_HEX_PREVIEW_BYTES ? row->length : TERMINAL_HEX_PREVIEW_BYTES;
        for (size_t i = 0; i < shown; ++i)
        {
            const uint8_t byte = static_cast<uint8_t>(row->data[i]);
            if (i != 0 && i % row->hex_columns == 0)
                output[pos++] = '\n';
            output[pos++] = digits[byte >> 4];
            output[pos++] = digits[byte & 15];
        }
        output[pos] = '\0';
        if (shown < row->length)
            snprintf(output + pos, 48, "\n(+%u bytes)", unsigned(row->length - shown));
    }
    else
    {
        size_t length = row->length;
        while (length && (row->data[length - 1] == '\r' || row->data[length - 1] == '\n'))
            --length;
        for (size_t i = 0; i < length; ++i)
            output[i] = row->data[i] == '\0' ? '.' : row->data[i];
        output[length] = '\0';
    }
    lv_label_set_recolor(label, false);
    // The label measures and wraps plain text; the draw callback supplies HEX colors.
    lv_obj_set_style_text_opa(label, terminal_hex ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    // Both buffers belong to the row; LVGL need not duplicate long HEX text.
    lv_label_set_text_static(label, output);
}

static void ui_record_terminal_line(bool is_rx, const char *text, size_t length)
{
    ui_term_new_line(is_rx);
    if (length > UART_LINE_MAX_LENGTH + 1)
        length = UART_LINE_MAX_LENGTH + 1;
    const size_t preview = length < TERMINAL_HEX_PREVIEW_BYTES ? length : TERMINAL_HEX_PREVIEW_BYTES;
    const size_t hex_capacity = 3 * preview + 48;
    const size_t display_capacity = length + 1 > hex_capacity ? length + 1 : hex_capacity;
    const size_t bytes = sizeof(terminal_row) + length + 1 + display_capacity;
    auto *row = static_cast<terminal_row *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (row == nullptr)
        row = static_cast<terminal_row *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (row == nullptr)
    {
        lv_label_set_text(currentLine, "[history allocation failed]");
        return;
    }
    row->length = length;
    memcpy(row->data, text, length);
    row->data[length] = '\0';
    lv_obj_set_user_data(currentLine, row);
    lv_obj_add_event_cb(currentLine, ui_terminal_row_delete, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(currentLine, ui_draw_terminal_hex, LV_EVENT_DRAW_MAIN, nullptr);
    ui_render_terminal_row(currentLine);
}

static void ui_toggle_terminal_format()
{
    terminal_hex = !terminal_hex;
    const lv_coord_t previous_scroll = lv_obj_get_scroll_y(ui_panList);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(ui_panList); ++i)
        ui_render_terminal_row(lv_obj_get_child(ui_panList, i));
    lv_obj_update_layout(ui_panList);
    if (panList_scroll_to_view)
        lv_obj_scroll_to_view(currentLine, LV_ANIM_OFF);
    else
        lv_obj_scroll_to_y(ui_panList, previous_scroll, LV_ANIM_OFF);
}

// A scrollable encoder target does not receive LV_EVENT_LONG_PRESSED in LVGL 8.
// There is one object in the group, so LVGL keeps editing enabled on a long hold.
static bool ui_terminal_button_event(lv_event_code_t code)
{
    if (code == LV_EVENT_PRESSED)
    {
        terminal_pressed_ms = millis();
        terminal_press_pending = true;
        terminal_pressed_in_chart = is_chart;
    }
    if (code == LV_EVENT_RELEASED)
    {
        const bool toggle = terminal_press_pending && !terminal_pressed_in_chart && !is_chart &&
                            uint32_t(millis() - terminal_pressed_ms) >= TERMINAL_LONG_PRESS_MS;
        terminal_press_pending = false;
        if (toggle)
        {
            ui_toggle_terminal_format();
            return true; // Do not also run the short-press scroll action.
        }
    }
    return false;
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
    bool is_new_data = false;
    bool is_new_line = true;
    bool is_end_with_new_line = false;
    uint16_t pos_start;
    String current_str;
    bool cuurent_is_rx;

    // chart
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
                if (q.data_len == 0)
                {
                    vTaskDelay(UI_REFRESH_DELAY);
                    continue;
                }

                is_new_data = true;
                rec(q.is_rx, q.data_char);

                if (pdTRUE == xSemaphoreTake(lvgl_mutex, portMAX_DELAY))
                {
                    // Capture raw bytes before strtok mutates the queue buffer.
                    ui_record_terminal_line(q.is_rx, q.data_char, q.data_len);

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
                            ui_star_shoot(q.is_rx, pch);

                            JsonDocument doc;
                            const DeserializationError error = deserializeJson(doc, pch);
                            const bool is_config_line = !error && doc.is<JsonObject>() && !doc["config"].isNull();
                            if (is_config_line)
                                ui_update_chart_config(doc);

                            // update ui
                            if (is_chart)
                            {
                                // attention: only JsonObject would be accepted
                                // {"a":123} ok
                                // [123,123] skip

                                if (is_config_line)
                                {
                                    // A control/configuration frame must not generate zero samples.
                                }
                                else if (error || !doc.is<JsonObject>())
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
                                        if (!chart_series_prop.is_set_by_config)
                                        {
                                            chart_series_prop.series_count = 0;
                                            for (JsonPairConst pair : doc.as<JsonObjectConst>())
                                            {
                                                if (chart_series_prop.series_count >= CHART_COUNT_MAX)
                                                    break;
                                                if (!pair.value().is<double>() || !std::isfinite(pair.value().as<double>()))
                                                    continue;
                                                chart_series_prop.sername[chart_series_prop.series_count++] = pair.key().c_str();
                                            }
                                        }
                                        ui_update_legend_layout();

                                        log_i("chart_series_names inited, count:%d", chart_series_prop.series_count);
                                        is_prepare_sername_needed = chart_series_prop.series_count == 0;
                                    }

                                    if (chart_series_prop.series_count == 0)
                                    {
                                        lv_label_set_text(ui_lblQString, "No numeric fields");
                                        if (!ui_panWaiting_is_visible)
                                        {
                                            lv_obj_fade_in(ui_panWaiting, 100, 0);
                                            ui_panWaiting_is_visible = true;
                                        }
                                    }
                                    else
                                    {
                                        // update chart
                                        for (size_t i = 0; i < chart_series_prop.series_count; i++)
                                        {
                                            JsonVariantConst value = doc[chart_series_prop.sername[i]];
                                            int32_t coordinate;
                                            if (!value.is<double>() ||
                                                !chart_validation::scale_value(value.as<double>(), chart_ranges[i].value_scale, coordinate))
                                            {
                                                lv_chart_set_next_value(ui_group_chart[i], ui_group_chart_series[i], LV_CHART_POINT_NONE);
                                                ui_set_legend_text(i, "--");
                                                continue;
                                            }
                                            ui_update_chart_range(ui_group_chart[i], &chart_ranges[i], coordinate);
                                            lv_chart_set_next_value(ui_group_chart[i], ui_group_chart_series[i], coordinate);
                                            ui_set_legend_number(i, value.as<double>());
                                        }

                                        // In shift mode, each new sample appears at the right edge.
                                        const lv_coord_t right_edge = lv_obj_get_content_width(ui_group_chart[0]) +
                                            lv_obj_get_style_pad_left(ui_group_chart[0], LV_PART_MAIN) +
                                            lv_obj_get_style_border_width(ui_group_chart[0], LV_PART_MAIN);
                                        lv_obj_set_x(ui_cursorV, right_edge + 14);

                                        // display the legend
                                        if (ui_panWaiting_is_visible)
                                        {
                                            lv_obj_fade_out(ui_panWaiting, 200, 0);
                                            ui_panWaiting_is_visible = false;
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            ui_star_shoot(q.is_rx, pch);
                            if (is_chart)
                                lv_label_set_text(ui_lblQString, pch);
                            if (strlen(current_str.c_str()) < TERM_LINE_MAX_LENGTH)
                                current_str += pch;
                            else
                            {
                                ui_term_new_line(q.is_rx);
                                current_str = pch;
                            }
                            lv_label_set_text(currentLine, current_str.c_str());
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
    while (lv_obj_get_child_cnt(ui_panList) >= TERM_ROW_MAX_NUM)
        lv_obj_del(lv_obj_get_child(ui_panList, 0));
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
    lv_label_set_text_fmt(ui_lblLineCounter, "%lu", (unsigned long)lineCount);
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
    if (ui_terminal_button_event(panList_event_code))
        return;
    if (panList_event_code == LV_EVENT_RELEASED)
    {
        if (is_chart)
        {
            // reset range
            log_i("reset %d chart_series range", chart_series_prop.series_count);
            for (size_t i = 0; i < chart_series_prop.series_count; i++)
            {
                lv_coord_t *ser_values = lv_chart_get_y_array(ui_group_chart[i], ui_group_chart_series[i]);
                if (chart_validation::find_range(ser_values, chart_point_count, lv_coord_t(LV_CHART_POINT_NONE),
                                                 chart_ranges[i].min, chart_ranges[i].max))
                {
                    chart_ranges[i].is_initialized = true;
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
    // LVGL sends ENTER after encoder release too; only rotation pauses following.
    const uint32_t key = panList_event_code == LV_EVENT_KEY ? lv_event_get_key(e) : 0;
    if (panList_event_code == LV_EVENT_KEY && panList_scroll_to_view &&
        (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN))
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
    static bool first_flush = true;
    if (first_flush)
    {
        first_flush = false;
        log_i("UI: first display flush submitted, heap=%lu", (unsigned long)ESP.getFreeHeap());
    }
}

static bool lv_port_init(void)
{
    log_i("UI: LCD begin, heap=%lu", (unsigned long)ESP.getFreeHeap());
    if (!display.begin())
    {
        log_e("UI: LCD initialization failed");
        return false;
    }
    log_i("UI: LCD initialized");
    display.setBrightness(0);
    display.setRotation(1); /* Landscape orientation, flipped */

    // lv_port
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DRAW_BUFFER_PIXELS);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    /*Change the following line to your display resolution*/
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = lv_my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    if (lv_disp_drv_register(&disp_drv) == nullptr)
    {
        log_e("UI: display registration failed");
        return false;
    }

    /*Initialize the input device driver for LVGL*/
    if (!test_mode)
    {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_ENCODER;
        indev_drv.read_cb = ui_hw_encoder_read;
        indev_encoder = lv_indev_drv_register(&indev_drv);
        group_encoder = lv_group_create();
        if (indev_encoder == nullptr || group_encoder == nullptr)
        {
            log_e("UI: input device or group allocation failed");
            return false;
        }
        lv_indev_set_group(indev_encoder, group_encoder);
    }
    log_i("UI: LVGL port ready, heap=%lu", (unsigned long)ESP.getFreeHeap());
    return true;
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
        lv_chart_set_all_value(ui_group_chart[i], ui_group_chart_series[i], LV_CHART_POINT_NONE);
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
        lv_chart_set_update_mode(ui_group_chart[i], LV_CHART_UPDATE_MODE_SHIFT);
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
    lv_obj_set_style_pad_all(ui_panSeries, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ui_panSeries, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (size_t i = 0; i < CHART_COUNT_MAX; i++)
    {
        ui_group_legend_pan[i] = lv_obj_create(ui_panSeries);
        lv_obj_set_width(ui_group_legend_pan[i], 0);
        lv_obj_set_flex_grow(ui_group_legend_pan[i], 1);
        lv_obj_set_flex_flow(ui_group_legend_pan[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ui_group_legend_pan[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(ui_group_legend_pan[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
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
        lv_obj_set_width(ui_group_legend_cap[i], lv_pct(100));
        lv_obj_set_height(ui_group_legend_cap[i], LV_SIZE_CONTENT); /// 1
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
        lv_obj_set_style_pad_bottom(ui_group_legend_cap[i], 2, LV_PART_MAIN | LV_STATE_DEFAULT);

        ui_group_legend_val[i] = lv_label_create(ui_group_legend_pan[i]);
        lv_obj_set_width(ui_group_legend_val[i], lv_pct(100));
        lv_obj_set_height(ui_group_legend_val[i], LV_SIZE_CONTENT); /// 1
        lv_label_set_long_mode(ui_group_legend_val[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(ui_group_legend_val[i], "--");
        lv_obj_set_style_text_color(ui_group_legend_val[i], lv_color_hex(COLOR_CHART_SERIES_VALUE_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_group_legend_val[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_group_legend_val[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(ui_group_legend_val[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
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

static uint32_t ui_startup_ms;
static bool ui_editing_enabled;

// Runs inside lv_timer_handler(), while lvgl_mutex is held by lv_task_timer.
static void ui_startup_tick(lv_timer_t *timer)
{
    if (lineCount > 0 && ui_imgSplash != nullptr)
    {
        lv_obj_del(ui_imgSplash);
        ui_imgSplash = nullptr;
    }
    if (!ui_editing_enabled && millis() - ui_startup_ms >= 5000)
    {
        lv_group_set_editing(group_encoder, true);
        ui_editing_enabled = true;
    }
    if (ui_imgSplash == nullptr && ui_editing_enabled)
        lv_timer_del(timer);
}

static void ui_start_animations(void)
{
    // Register animations before any UI worker is started.
    lv_obj_set_style_opa(ui_panContainerChart, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui_panInfo);
    lv_anim_set_delay(&animation, 359);
    lv_anim_set_time(&animation, 359);
    lv_anim_set_exec_cb(&animation, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_values(&animation, -27, 0);
    lv_anim_start(&animation);
    ui_startup_ms = millis();
    ui_editing_enabled = false;
    if (lv_timer_create(ui_startup_tick, 50, nullptr) == nullptr)
    {
        log_e("UI: startup timer allocation failed");
        lv_obj_del(ui_imgSplash);
        ui_imgSplash = nullptr;
        lv_group_set_editing(group_encoder, true);
        ui_editing_enabled = true;
    }
}

static bool ui_create_task(TaskFunction_t task, const char *name, UBaseType_t priority)
{
    if (xTaskCreatePinnedToCore(task, name, 8192, nullptr, priority, nullptr, 1) == pdPASS)
        return true;
    log_e("UI: task creation failed: %s, heap=%lu", name, (unsigned long)ESP.getFreeHeap());
    return false;
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

    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == nullptr)
    {
        log_e("UI: mutex allocation failed");
        return;
    }
    new_gamma_table(LCD_BKL_GAMMA);
    if (!lv_port_init())
        return;

    if (test_mode)
        ui_draw_main_screen_test_mode();
    else
    {
        ui_draw_main_screen();
        ui_start_animations();
    }
    log_i("UI: screen loaded, mode=%s, heap=%lu", test_mode ? "test" : "normal",
          (unsigned long)ESP.getFreeHeap());

    if (!ui_create_task(lv_task_timer, "lv timer", 9))
        return;
    if (test_mode)
        ui_create_task(ui_task_update_main_screen_test_mode, "update main screen test mode", 5);
    else
        ui_create_task(ui_task_update_main_screen, "update main screen", 5);
    ui_create_task(hw_task_backlight, "update backlight", 9);
    ui_create_task(ui_task_update_topbar, "update top info bar", 1);
    ui_create_task(ui_task_toggle_chart_terminal, "toggle chart terminal", 1);
}
