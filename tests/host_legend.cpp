#include "lvgl.h"
#include <ArduinoJson.h>
#include "chart_timeline.h"
#include "network_time.h"
#include "chart_validation.h"
#include <ctime>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <cstdlib>
#include <set>
#include <map>
#include <vector>
#include <algorithm>

using String = std::string;
constexpr int CHART_COUNT_MAX = 5;
constexpr int CHART_VALUE_SCALE_DEFAULT = 100;
#include "ui_structs.inc"
static chart_series chart_series_prop{};
static chart_range chart_ranges[CHART_COUNT_MAX];
static lv_obj_t *ui_group_chart[CHART_COUNT_MAX];
static lv_chart_series_t *ui_group_chart_series[CHART_COUNT_MAX];
static lv_obj_t *ui_cursorV;
static constexpr uint16_t chart_point_count = 137;
static long terminal_released_millis = 0;
#define log_i(...) ((void)0)
static unsigned char preview_pixels[320 * 240 * 3];
static lv_obj_t *ui_panContainerChart;
static lv_obj_t *ui_panSeries;
static lv_obj_t *ui_group_legend_pan[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_cap[CHART_COUNT_MAX];
static lv_obj_t *ui_group_legend_val[CHART_COUNT_MAX];
constexpr int COLOR_SHADOW = 0;
constexpr int COLOR_HIGHLIGHT = 0xFFFef7;
constexpr int CHART_WIDTH_PIXEL = 274;
static lv_obj_t *ui_panCharts;
static lv_obj_t *ui_lblChartSpan;
static lv_obj_t *ui_lblChartTimestamp;
static int64_t test_epoch_base = 0;
int64_t network_epoch_us(uint64_t us) { return test_epoch_base ? test_epoch_base + us : 0; }
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
static bool is_chart = false;
static bool panList_scroll_to_view = true;
constexpr size_t UART_LINE_MAX_LENGTH = 2048;
constexpr int MALLOC_CAP_SPIRAM = 1;
constexpr int MALLOC_CAP_8BIT = 2;
static std::set<void *> terminal_allocations;
static void *heap_caps_malloc(size_t size, int)
{
    void *ptr = malloc(size);
    if (ptr) terminal_allocations.insert(ptr);
    return ptr;
}
static void terminal_free(void *ptr)
{
    if (ptr) assert(terminal_allocations.erase(ptr) == 1);
    free(ptr);
}
static uint32_t terminal_test_ms = 0;
static uint32_t millis() { return terminal_test_ms; }
#define free terminal_free
#include "terminal_helpers.inc"
#undef free
#include "clock_helpers.inc"
#include "plot_helpers.inc"
#include "terminal_events.inc"

static lv_obj_t *ui_lblNetwork = nullptr, *ui_lblNetworkStatus = nullptr;
static lv_obj_t *ui_panInfo = nullptr;
static NetworkState test_network_state = NetworkState::Unconfigured;
NetworkState network_state() { return test_network_state; }
constexpr int COLOR_INFO_TEXT = 0xFFFef7;
#include "network_indicator.inc"

static void check_network_indicator()
{
    // Real widgets and partial redraws, without changing pages or forcing a full refresh.
    ui_panInfo = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(ui_panInfo);
    lv_obj_set_size(ui_panInfo, 320, 27);
    lv_obj_set_style_bg_color(ui_panInfo, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_panInfo, LV_OPA_COVER, LV_PART_MAIN);
#include "network_creation.inc"
    lv_obj_update_layout(ui_panInfo);
    auto *slot = lv_obj_get_parent(ui_lblNetwork);
    assert(lv_obj_get_width(slot) == 18 && lv_obj_get_height(slot) == 18);
    assert(lv_obj_get_style_text_font(ui_lblNetwork, LV_PART_MAIN) == &lv_font_montserrat_12);
    assert(lv_obj_get_style_text_font(ui_lblNetworkStatus, LV_PART_MAIN) == &lv_font_montserrat_8);
    auto snapshot = [&]() {
        lv_area_t area;
        lv_obj_get_coords(slot, &area);
        std::vector<unsigned char> pixels;
        for (int y = area.y1; y <= area.y2; ++y)
            for (int x = area.x1; x <= area.x2; ++x)
                for (int c = 0; c < 3; ++c)
                    pixels.push_back(preview_pixels[(y * 320 + x) * 3 + c]);
        return pixels;
    };
    test_network_state = NetworkState::Online;
    ui_update_network_indicator();
    lv_refr_now(nullptr);
    const auto online_pixels = snapshot();
    for (const auto status : {NetworkState::Provisioning, NetworkState::Connecting,
                              NetworkState::Online, NetworkState::Failed, NetworkState::Online})
    {
        test_network_state = status;
        ui_update_network_indicator();
        lv_refr_now(nullptr);
        const char *expected = status == NetworkState::Online ? "" :
            status == NetworkState::Provisioning ? "?" :
            status == NetworkState::Connecting ? "..." : "x";
        assert(std::string(lv_label_get_text(ui_lblNetworkStatus)) == expected);
        if (status == NetworkState::Online) assert(snapshot() == online_pixels);
        else assert(snapshot() != online_pixels);
    }
    lv_obj_del(ui_panInfo);
    ui_panInfo = ui_lblNetwork = ui_lblNetworkStatus = nullptr;
    ui_update_network_indicator(); // Test mode has no network widgets.
}

static std::string terminal_visible_text(lv_obj_t *label)
{
    assert(!lv_label_get_recolor(label));
    return lv_label_get_text(label);
}

static std::map<std::pair<int, int>, std::pair<uint32_t, uint32_t>> drawn_hex;
static lv_area_t capture_area;
static void (*original_draw_letter)(lv_draw_ctx_t *, const lv_draw_label_dsc_t *, const lv_point_t *, uint32_t);
static void capture_hex_letter(lv_draw_ctx_t *context, const lv_draw_label_dsc_t *draw,
                               const lv_point_t *position, uint32_t letter)
{
    if (draw->font == &ui_font_SFMono_14 && position->x >= capture_area.x1 &&
        position->x <= capture_area.x2 && position->y >= capture_area.y1 && position->y <= capture_area.y2)
    {
        const auto key = std::make_pair(int(position->y), int(position->x));
        const auto value = std::make_pair(letter, uint32_t(draw->color.full));
        const auto found = drawn_hex.find(key);
        assert(found == drawn_hex.end() || found->second == value);
        drawn_hex[key] = value;
    }
    original_draw_letter(context, draw, position, letter);
}

static void check_wrapped_hex_rendering()
{
    lv_obj_add_flag(ui_panContainerChart, LV_OBJ_FLAG_HIDDEN);
    auto *context = lv_disp_get_default()->driver->draw_ctx;
    original_draw_letter = context->draw_letter;
    for (bool rx : {true, false})
    {
        terminal_hex = true;
        ui_panList = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ui_panList, 80, 210);
        lv_obj_set_pos(ui_panList, 0, 0);
        lv_obj_set_style_pad_all(ui_panList, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui_panList, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui_panList, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui_panList, LV_OPA_COVER, LV_PART_MAIN);
        const std::string sample(81, char(0xFF));
        ui_record_terminal_line(rx, sample.data(), sample.size());
        const std::string text = lv_label_get_text(currentLine);
        assert(text.find('#') == std::string::npos && text.find(' ') == std::string::npos);
        assert(std::count(text.begin(), text.end(), 'F') == 162);
        assert(std::count(text.begin(), text.end(), '\n') > 1);
        const auto *row = static_cast<terminal_row *>(lv_obj_get_user_data(currentLine));
        assert(row->hex_columns > 1 && row->hex_columns < sample.size());
        const auto *font = lv_obj_get_style_text_font(currentLine, LV_PART_MAIN);
        const int spacing = lv_obj_get_style_text_letter_space(currentLine, LV_PART_MAIN);
        const int glyph_width = lv_font_get_glyph_width(font, 'F', 'F');
        const int glyph_step = glyph_width + spacing;
        const int line_step = lv_font_get_line_height(font) + lv_obj_get_style_text_line_space(currentLine, LV_PART_MAIN);
        for (int scroll : {0, 120})
        {
            lv_obj_scroll_to_y(ui_panList, scroll, LV_ANIM_OFF);
            lv_obj_update_layout(ui_panList);
            lv_obj_get_content_coords(currentLine, &capture_area);
            drawn_hex.clear();
            context->draw_letter = capture_hex_letter;
            lv_obj_invalidate(ui_panList);
            lv_refr_now(nullptr);
            context->draw_letter = original_draw_letter;
            assert(!drawn_hex.empty());
            unsigned deepest_line = 0;
            for (const auto &entry : drawn_hex)
            {
                const int y = entry.first.first, x = entry.first.second;
                const unsigned line = (y - capture_area.y1) / line_step;
                const unsigned first = line * row->hex_columns;
                const unsigned bytes = std::min(unsigned(row->hex_columns), unsigned(sample.size()) - first);
                const int line_width = 2 * bytes * glyph_step - spacing;
                const int start_x = rx ? capture_area.x1 : capture_area.x2 - line_width + 1;
                if ((x - start_x) % glyph_step != 0)
                    std::cerr << "RX=" << rx << " scroll=" << scroll << " x=" << x << " start=" << start_x
                              << " y=" << y << " line=" << line << " char=" << entry.second.first
                              << " step=" << glyph_step << std::endl;
                assert((x - start_x) % glyph_step == 0);
                const unsigned nibble = (x - start_x) / glyph_step;
                assert(nibble < bytes * 2 && entry.second.first == 'F');
                const unsigned byte = first + nibble / 2;
                const lv_color_t primary = lv_color_hex(rx ? COLOR_TERM_RX_TEXT : COLOR_TERM_TX_TEXT);
                const lv_color_t alternate = lv_color_hex(rx ? 0x6AC5DB : COLOR_TERM_RX_TEXT);
                assert(entry.second.second == uint32_t((byte % 2 == 0 ? primary : alternate).full));
                deepest_line = std::max(deepest_line, line);
            }
            assert(deepest_line > 1); // Every rendered line must preserve byte parity.
        }
        lv_obj_del(ui_panList);
        assert(terminal_allocations.empty());
    }
}

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
        ui_record_terminal_line(i % 2 == 0, text, strlen(text));
        assert(lv_obj_get_child_cnt(ui_panList) <= TERM_ROW_MAX_NUM);
    }
    assert(lineCount == 100);
    assert(std::string(lv_label_get_text(ui_lblLineCounter)) == "100");
    assert(lv_obj_get_child_cnt(ui_panList) == TERM_ROW_MAX_NUM);
    assert(std::string(lv_label_get_text(lv_obj_get_child(ui_panList, 0))) == "packet 50");
    assert(std::string(lv_label_get_text(currentLine)) == "packet 99");
    lv_obj_add_flag(ui_panList, LV_OBJ_FLAG_HIDDEN);
    ui_record_terminal_line(true, "received with terminal hidden", strlen("received with terminal hidden"));
    assert(lineCount == 101 && lv_obj_get_child_cnt(ui_panList) == TERM_ROW_MAX_NUM);
    lv_obj_clear_flag(ui_panList, LV_OBJ_FLAG_HIDDEN);
    assert(std::string(lv_label_get_text(currentLine)) == "received with terminal hidden");
    // A format change re-renders the cached bytes even without new UART input.
    const char bytes[] = {'A', '\0', char(0x80), char(0xFF), '\r'};
    ui_record_terminal_line(false, bytes, sizeof(bytes));
    assert(std::string(lv_label_get_text(currentLine)) == std::string("A.\x80\xFF", 4));
    const uint32_t count = lineCount;
    assert(!ui_terminal_button_event(LV_EVENT_PRESSED));
    terminal_test_ms += 2100;
    assert(ui_terminal_button_event(LV_EVENT_RELEASED));
    assert(terminal_hex && lineCount == count);
    assert(terminal_visible_text(currentLine) == "410080FF0D");
    assert(!lv_label_get_recolor(currentLine));
    assert(std::string(lv_label_get_text(currentLine)).find('#') == std::string::npos);
    assert(std::string(lv_label_get_text(currentLine)).find(' ') == std::string::npos);
    assert(!ui_terminal_button_event(LV_EVENT_RELEASED));
    assert(terminal_hex); // duplicate release cannot toggle twice
    ui_terminal_button_event(LV_EVENT_PRESSED);
    terminal_test_ms += 1999;
    assert(!ui_terminal_button_event(LV_EVENT_RELEASED));
    assert(terminal_hex); // short press is left for the existing scroll action
    is_chart = true;
    ui_terminal_button_event(LV_EVENT_PRESSED);
    terminal_test_ms += 1000;
    assert(!ui_terminal_button_event(LV_EVENT_RELEASED));
    assert(terminal_hex);
    is_chart = false;
    std::string longest(UART_LINE_MAX_LENGTH, char(0xFF));
    longest += '\n';
    ui_record_terminal_line(true, longest.data(), longest.size());
    assert(terminal_visible_text(currentLine).size() < 560);
    assert(terminal_visible_text(currentLine).find("(+1793 bytes)") != std::string::npos);
    auto *cached = static_cast<terminal_row *>(lv_obj_get_user_data(currentLine));
    assert(cached->length == longest.size());
    assert(memcmp(cached->data, longest.data(), longest.size()) == 0);
    assert(terminal_visible_text(currentLine).substr(0, 8) == "FFFFFFFF");
    ui_toggle_terminal_format();
    assert(!terminal_hex && lineCount == count + 1);
    assert(strlen(lv_label_get_text(currentLine)) == UART_LINE_MAX_LENGTH);
    assert(terminal_allocations.size() == TERM_ROW_MAX_NUM);
    lv_obj_add_event_cb(ui_panList, ui_event_panList, LV_EVENT_ALL, nullptr);
    panList_scroll_to_view = true;
    const uint32_t count_before_press = lineCount;
    lv_event_send(ui_panList, LV_EVENT_PRESSED, nullptr);
    terminal_test_ms += 2100;
    lv_event_send(ui_panList, LV_EVENT_RELEASED, nullptr);
    assert(terminal_hex && panList_scroll_to_view);
    uint32_t key = LV_KEY_ENTER;
    lv_event_send(ui_panList, LV_EVENT_KEY, &key);
    assert(panList_scroll_to_view); // LVGL's trailing ENTER is not a scroll request.
    key = LV_KEY_RIGHT;
    lv_event_send(ui_panList, LV_EVENT_KEY, &key);
    assert(!panList_scroll_to_view);
    lv_event_send(ui_panList, LV_EVENT_PRESSED, nullptr);
    terminal_test_ms += 100;
    lv_event_send(ui_panList, LV_EVENT_RELEASED, nullptr);
    assert(panList_scroll_to_view && lineCount == count_before_press);
    for (unsigned i = 0; i < TERM_ROW_MAX_NUM; ++i)
        ui_record_terminal_line(true, longest.data(), longest.size());
    lv_obj_update_layout(ui_panList);
    assert(lv_obj_get_scroll_bottom(ui_panList) >= 0);
    lv_area_t last_row;
    lv_obj_get_coords(currentLine, &last_row);
    assert(last_row.y2 < 30000); // Wrapped previews remain within LVGL's coordinate range.
    assert(terminal_allocations.size() == TERM_ROW_MAX_NUM);
    lv_obj_del(ui_panList);
    assert(terminal_allocations.empty());
    terminal_hex = false;
    ui_panList = lv_obj_create(lv_scr_act());
    const char *literal = "#FF0000 literal#";
    ui_record_terminal_line(true, literal, strlen(literal));
    assert(!lv_label_get_recolor(currentLine));
    assert(terminal_visible_text(currentLine) == literal);
    ui_toggle_terminal_format();
    assert(!lv_label_get_recolor(currentLine));
    assert(terminal_visible_text(currentLine).substr(0, 4) == "2346");
    ui_toggle_terminal_format();
    assert(!lv_label_get_recolor(currentLine) && terminal_visible_text(currentLine) == literal);
    lv_obj_clean(ui_panList);
    lv_obj_set_size(ui_panList, 320, 80);
    lv_obj_set_pos(ui_panList, 0, 0);
    lv_obj_set_style_pad_all(ui_panList, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_panList, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_panList, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_panList, LV_OPA_COVER, LV_PART_MAIN);
    terminal_hex = true;
    const std::string sample(16, char(0xFF));
    ui_record_terminal_line(true, sample.data(), sample.size());
    lv_obj_update_layout(ui_panList);
    lv_refr_now(nullptr);
    lv_area_t area;
    lv_obj_get_coords(currentLine, &area);
    unsigned found_primary = 0, found_alternate = 0;
    const uint32_t primary = lv_color_to32(lv_color_hex(COLOR_TERM_RX_TEXT)) & 0xFFFFFF;
    const uint32_t alternate = lv_color_to32(lv_color_hex(0x6AC5DB)) & 0xFFFFFF;
    for (int y = area.y1; y <= area.y2 && y < 240; ++y)
        for (int x = area.x1; x <= area.x2 && x < 320; ++x)
        {
            if (x < 0 || y < 0) continue;
            const auto *pixel = preview_pixels + (y * 320 + x) * 3;
            const uint32_t rgb = (uint32_t(pixel[0]) << 16) | (uint32_t(pixel[1]) << 8) | pixel[2];
            found_primary += rgb == primary;
            found_alternate += rgb == alternate;
        }
    assert(found_primary > 0 && found_alternate > 0);
    lv_obj_del(ui_panList);
    assert(terminal_allocations.empty());

}

static void create_actual_legend()
{
    ui_panCharts = lv_obj_create(ui_panContainerChart);
    lv_obj_set_size(ui_panCharts, 300, 150);
    lv_obj_set_style_pad_all(ui_panCharts, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_panCharts, 0, LV_PART_MAIN);
#include "clock_creation.inc"
#include "legend_creation.inc"
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
        {
            const uint32_t color = lv_color_to32(*colors++);
            const size_t pos = (y * 320 + x) * 3;
            preview_pixels[pos] = color >> 16;
            preview_pixels[pos + 1] = color >> 8;
            preview_pixels[pos + 2] = color;
        }
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

static void check_plot_stream()
{
    lv_obj_clear_flag(ui_panContainerChart, LV_OBJ_FLAG_HIDDEN);
    const char *names[] = {"ecg", "pressure", "response", "steps", "jitter"};
    chart_series_prop.series_count = CHART_COUNT_MAX;
    for (unsigned i = 0; i < CHART_COUNT_MAX; ++i)
    {
        chart_series_prop.sername[i] = names[i];
        chart_series_prop.value_fmt[i] = i == 0 ? "%.2f" : "%.1f";
        chart_ranges[i] = {};
        ui_group_chart[i] = lv_chart_create(ui_panCharts);
        lv_obj_set_size(ui_group_chart[i], 274, 124);
        lv_obj_align(ui_group_chart[i], LV_ALIGN_CENTER, 0, 0);
        lv_chart_set_point_count(ui_group_chart[i], chart_point_count);
        lv_chart_set_update_mode(ui_group_chart[i], LV_CHART_UPDATE_MODE_SHIFT);
        lv_obj_set_style_bg_opa(ui_group_chart[i], LV_OPA_TRANSP, LV_PART_MAIN);
        ui_group_chart_series[i] = lv_chart_add_series(ui_group_chart[i], lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    }
    ui_cursorV = lv_obj_create(ui_panCharts);
    lv_obj_set_size(ui_cursorV, 2, 124);
    ui_update_legend_layout();
    ui_reset_chart_timeline();
    for (unsigned sample = 0; sample < 5000; ++sample)
    {
        JsonDocument doc;
        doc["ecg"] = (int(sample % 201) - 100) / 100.0;
        doc["pressure"] = 50 + (sample % 601) / 10.0;
        doc["response"] = (sample % 1001) / 10.0;
        doc["steps"] = (sample % 64) / 8 * (100.0 / 7);
        doc["jitter"] = (int(sample % 161) - 80) / 10.0;
        if (sample % 19 == 0) doc.remove("jitter");
        if (sample % 23 == 0) doc["response"] = "missing";
        if (sample % 97 == 0) doc["ecg"] = 1e300;
        ui_plot_sample(doc, uint64_t(sample) * 300000);
        if (sample % 8 == 0) lv_refr_now(nullptr);
        if (sample % 512 == 0)
        {
            lv_obj_add_flag(ui_panContainerChart, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(nullptr);
            lv_obj_clear_flag(ui_panContainerChart, LV_OBJ_FLAG_HIDDEN);
            ui_update_legend_layout();
        }
    }
    lv_refr_now(nullptr);
    assert(chart_timeline.span_us() == uint64_t(chart_point_count - 1) * 300000);
    assert(!lv_obj_has_flag(ui_lblChartTimestamp, LV_OBJ_FLAG_HIDDEN));
    assert_uniform_layout(5);
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
    ui_reset_chart_timeline();
    assert(lv_obj_has_flag(ui_lblChartTimestamp, LV_OBJ_FLAG_HIDDEN));
    chart_timeline.push(0, 0);
    chart_timeline.push(390000000, 0);
    ui_update_chart_time_labels();
    assert(std::string(lv_label_get_text(ui_lblChartSpan)) == "-6.5min");
    assert(lv_obj_has_flag(ui_lblChartTimestamp, LV_OBJ_FLAG_HIDDEN));
    test_epoch_base = 1704067200000000LL;
    ui_update_chart_time_labels();
    assert(std::string(lv_label_get_text(ui_lblChartTimestamp)) == "08:06:30");
    assert(!lv_obj_has_flag(ui_lblChartTimestamp, LV_OBJ_FLAG_HIDDEN));
    test_epoch_base += 10000000;
    ui_update_chart_time_labels();
    assert(std::string(lv_label_get_text(ui_lblChartTimestamp)) == "08:06:30");
    lv_obj_update_layout(ui_panCharts);
    lv_area_t chart_area, left, right;
    lv_obj_get_coords(ui_panCharts, &chart_area);
    lv_obj_get_coords(ui_lblChartSpan, &left);
    lv_obj_get_coords(ui_lblChartTimestamp, &right);
    assert(left.x1 >= chart_area.x1 && right.x2 <= chart_area.x2);
    assert(left.x2 < right.x1 && left.y2 == right.y2 && right.y2 < chart_area.y2);
    ui_reset_chart_timeline();
    assert(std::string(lv_label_get_text(ui_lblChartSpan)) == "--");
    assert(lv_obj_has_flag(ui_lblChartTimestamp, LV_OBJ_FLAG_HIDDEN));
    check_plot_stream();
    check_terminal_history();
    check_wrapped_hex_rendering();
    check_network_indicator();
    // LVGL 8 removes the display objects but leaves the driver-owned draw
    // context to its caller. Release both before Linux LeakSanitizer runs.
    lv_disp_remove(lv_disp_get_default());
    if (driver.draw_ctx_deinit) driver.draw_ctx_deinit(&driver, driver.draw_ctx);
    lv_mem_free(driver.draw_ctx);
    driver.draw_ctx = nullptr;
    assert(terminal_allocations.empty());
    std::cout << "PASS: compact Wi-Fi icon and provisioning/connecting/online/offline corner transitions without page switching\n";
    std::cout << "PASS: real LVGL layout, uniform row fonts, centered columns, 1-5 channels, negative signs and wide-number fallback\n";
    std::cout << "PASS: actual five-channel chart stream, 5000 samples, missing/overflow values, time labels and repeated visibility changes under ASan\n";
    std::cout << "PASS: plain HEX bytes, direct alternating colors across wrapped/scrolled RX/TX lines, no markup glyphs\n";
    std::cout << "PASS: terminal history, ASCII/HEX cached redraw, long/short press separation, 2048-byte rows, bounded preview and row cleanup\n";
}
