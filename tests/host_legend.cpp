#include "lvgl.h"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using String = std::string;
constexpr int CHART_COUNT_MAX = 5;
constexpr int CHART_VALUE_SCALE_DEFAULT = 100;
#include "ui_structs.inc"
static chart_series chart_series_prop{};
static lv_obj_t *ui_panContainerChart;
static lv_obj_t *ui_panSeries;
static lv_obj_t *ui_group_legend_pan[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_cap[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_val[CHART_COUNT_MAX];
constexpr int COLOR_SHADOW = 0;
constexpr int COLOR_CHART_SERIES_NAME_TEXT = 0;
constexpr int COLOR_CHART_SERIES_VALUE_TEXT = 0xFFFef7;
constexpr int COLOR_TERM_RX_TEXT = 0xFFFef7;
constexpr int COLOR_TERM_TX_TEXT = 0xF5BD34;
constexpr int TERM_ROW_MAX_NUM = 50;
static lv_obj_t *ui_panList;
static lv_obj_t *currentLine;
static lv_obj_t *ui_lblLineCounter;
static uint32_t lineCount = 0;
extern "C" {
LV_FONT_DECLARE(ui_font_SFMono_23);
LV_FONT_DECLARE(ui_font_SFMono_14);
LV_FONT_DECLARE(ui_font_SFMono_9);
}

#include "legend_helpers.inc"
#include "terminal_helpers.inc"

static void check_terminal_history()
{
    ui_panList = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui_panList, 320, 200);
    ui_lblLineCounter = lv_label_create(lv_scr_act());
    // This path has no dependency on the selected page: chart and terminal
    // packets use the same append function before the view-specific branch.
    for (unsigned i = 0; i < 100; ++i)
    {
        char text[32];
        snprintf(text, sizeof(text), "packet %u", i);
        ui_record_terminal_line(i % 2 == 0, text);
        assert(lv_obj_get_child_cnt(ui_panList) <= TERM_ROW_MAX_NUM);
    }
    assert(lineCount == 100);
    assert(std::string(lv_label_get_text(ui_lblLineCounter)) == "100");
    assert(lv_obj_get_child_cnt(ui_panList) == TERM_ROW_MAX_NUM);
    assert(std::string(lv_label_get_text(lv_obj_get_child(ui_panList, 0))) == "packet 50");
    assert(std::string(lv_label_get_text(currentLine)) == "packet 99");
    lv_obj_add_flag(ui_panList, LV_OBJ_FLAG_HIDDEN);
    ui_record_terminal_line(true, "received with terminal hidden");
    assert(lineCount == 101 && lv_obj_get_child_cnt(ui_panList) == TERM_ROW_MAX_NUM);
    lv_obj_clear_flag(ui_panList, LV_OBJ_FLAG_HIDDEN);
    assert(std::string(lv_label_get_text(currentLine)) == "received with terminal hidden");
}

static void create_actual_legend()
{
#include "legend_creation.inc"
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *)
{
    lv_disp_flush_ready(driver);
}

static void assert_fits(size_t index, const char *expected)
{
    lv_obj_t *label = ui_group_legend_val[index];
    if (std::string(lv_label_get_text(label)) != expected)
        std::cerr << "channels=" << chart_series_prop.series_count << " index=" << index
                  << " width=" << lv_obj_get_content_width(label) << " expected=" << expected
                  << " actual=" << lv_label_get_text(label) << std::endl;
    assert(std::string(lv_label_get_text(label)) == expected);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const lv_coord_t width = lv_txt_get_width(expected, strlen(expected), font,
        lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), LV_TEXT_FLAG_NONE);
    assert(width <= lv_obj_get_content_width(label));
    lv_font_glyph_dsc_t glyph{};
    assert(lv_font_get_glyph_dsc(font, &glyph, '-', 0) && glyph.box_w > 0);
    lv_area_t parent, child;
    lv_obj_get_coords(ui_panSeries, &parent);
    lv_obj_get_coords(ui_group_legend_pan[index], &child);
    assert(child.x1 >= parent.x1 && child.x2 <= parent.x2);
}

static void assert_uniform_layout(unsigned count)
{
    const lv_font_t *caption_font = lv_obj_get_style_text_font(ui_group_legend_cap[0], LV_PART_MAIN);
    const lv_font_t *value_font = lv_obj_get_style_text_font(ui_group_legend_val[0], LV_PART_MAIN);
    lv_area_t caption_row, value_row;
    lv_obj_get_coords(ui_group_legend_cap[0], &caption_row);
    lv_obj_get_coords(ui_group_legend_val[0], &value_row);
    for (unsigned i = 0; i < count; ++i)
    {
        assert(lv_obj_get_style_text_font(ui_group_legend_cap[i], LV_PART_MAIN) == caption_font);
        assert(lv_obj_get_style_text_font(ui_group_legend_val[i], LV_PART_MAIN) == value_font);
        lv_area_t panel, caption, value;
        lv_obj_get_coords(ui_group_legend_pan[i], &panel);
        lv_obj_get_coords(ui_group_legend_cap[i], &caption);
        lv_obj_get_coords(ui_group_legend_val[i], &value);
        assert(caption.y1 == caption_row.y1 && caption.y2 == caption_row.y2);
        assert(value.y1 == value_row.y1 && value.y2 == value_row.y2);
        assert(abs(caption.x1 + caption.x2 - panel.x1 - panel.x2) <= 1);
        assert(abs(value.x1 + value.x2 - panel.x1 - panel.x2) <= 1);
        assert(abs(caption.y1 + value.y2 - panel.y1 - panel.y2) <= 2);
        assert(value.y1 - caption.y2 - 1 == 4);
    }
}

int main()
{
    lv_init();
    static lv_color_t buffer[320 * 32];
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, buffer, nullptr, 320 * 32);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 320;
    driver.ver_res = 240;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush;
    assert(lv_disp_drv_register(&driver));
    ui_panContainerChart = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui_panContainerChart, 320, 213);
    lv_obj_set_style_pad_all(ui_panContainerChart, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_panContainerChart, 0, LV_PART_MAIN);
    create_actual_legend();
    const char *names[] = {"ecg", "pressure", "response", "steps", "jitter"};
    const double values[] = {-0.25, 62.0, 100.0, 100.0, -8.0};
    const char *expected[] = {"-0.25", "62.0", "100.0", "100.0", "-8.0"};
    for (unsigned count : {5, 2, 1, 4, 3, 5})
    {
        chart_series_prop.series_count = count;
        for (unsigned i = 0; i < count; ++i)
        {
            chart_series_prop.sername[i] = names[i];
            chart_series_prop.value_fmt[i] = i == 0 ? "%.2f" : "%.1f";
        }
        ui_update_legend_layout();
        for (unsigned i = 0; i < count; ++i)
            ui_set_legend_number(i, values[i]);
        lv_obj_update_layout(ui_panSeries);
        for (unsigned i = 0; i < count; ++i)
            assert_fits(i, expected[i]);
        assert_uniform_layout(count);
    }
    chart_series_prop.value_fmt[0] = "%.3f";
    ui_set_legend_number(0, -1000000.0);
    lv_obj_update_layout(ui_panSeries);
    assert_fits(0, "-1.00e+06");
    assert_uniform_layout(5);
    ui_set_legend_text(0, "--");
    lv_obj_update_layout(ui_panSeries);
    assert_fits(0, "--");
    assert_uniform_layout(5);
    check_terminal_history();
    std::cout << "PASS: real LVGL layout, uniform row fonts, centered columns, 1-5 channels, negative signs and wide-number fallback\n";
    std::cout << "PASS: terminal history continues while hidden, keeps newest 50 records and counts each line once\n";
}
