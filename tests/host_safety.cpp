#include <ArduinoJson.h>
#include "chart_validation.h"
#include "recording_helpers.h"
#include "avg.h"
#include <cassert>
#include <climits>
#include <iostream>
#include <limits>
#include <string>

// Only device APIs are stubbed. Structs and functions below are extracted from
// the firmware source by run_host_checks.py; input parsing uses real ArduinoJson.
using String = std::string;
using lv_coord_t = int32_t;
constexpr int CHART_COUNT_MAX = 5;
constexpr uint32_t CHART_VALUE_SCALE_DEFAULT = 100;
constexpr const char *CHART_VALUE_FMT_DEFAULT = "%.1f";
constexpr int LV_CHART_AXIS_PRIMARY_Y = 0;
constexpr int32_t LV_CHART_POINT_NONE = INT32_MAX;
#define log_i(...) ((void)0)
#define log_w(...) ((void)0)
#define log_e(...) ((void)0)

#include "ui_structs.inc"

struct Chart { int32_t low = 0, high = 100; } charts[5];
using lv_obj_t = Chart;
Chart *ui_group_chart[5] = {&charts[0], &charts[1], &charts[2], &charts[3], &charts[4]};
void *ui_group_chart_series[5] = {};
void *ui_cursorV = nullptr;
chart_range chart_ranges[CHART_COUNT_MAX];
chart_series chart_series_prop{};
uint32_t chart_palette[5] = {1, 2, 3, 4, 5};
bool is_prepare_sername_needed = false;
bool is_reset_chart_color_needed = false;
int clears = 0, color_updates = 0;
void lv_chart_set_range(Chart *chart, int, int32_t low, int32_t high) { chart->low = low; chart->high = high; }
void lv_chart_set_all_value(Chart *, void *, int32_t) { ++clears; }
void lv_obj_set_x(void *, int) {}
void ui_update_chart_color() { ++color_updates; is_reset_chart_color_needed = false; }

struct lv_timer_t {};
uint32_t lineCount = 0, ui_startup_ms = 0, now_ms = 0;
bool ui_editing_enabled = false;
void *ui_imgSplash = nullptr, *group_encoder = nullptr;
int deleted_splashes = 0, deleted_timers = 0;
uint32_t millis() { return now_ms; }
void lv_obj_del(void *) { ++deleted_splashes; }
void lv_group_set_editing(void *, bool enabled) { assert(enabled); }
void lv_timer_del(lv_timer_t *) { ++deleted_timers; }

struct PreferencesStub {
    bool open_ok = true;
    size_t written = 1;
    int writes = 0, closes = 0;
    bool begin(const char *, bool) { return open_ok; }
    size_t putUChar(const char *, uint8_t) { ++writes; return written; }
    void end() { ++closes; }
} preferences;
uint8_t bandrate_index = 7, bandrate_index_in_nvs = 7;

#include "ui_functions.inc"

static void config(const char *json)
{
    JsonDocument document;
    assert(!deserializeJson(document, json));
    ui_update_chart_config(document);
}

static void check_chart_inputs()
{
    using namespace chart_validation;
    for (const char *text : {"%.0f", "%.1f", "%.2f", "%.3f"}) assert(valid_format(text));
    for (const char *text : {"", "%s", "%n", "%*f", "%f%f", "%.99999f", "value: %.1f"}) assert(!valid_format(text));
    assert(!valid_format(nullptr));
    uint32_t color = 123;
    assert(parse_color("0x00FF00", color) && color == 0x00FF00);
    assert(parse_color("#ff4d6d", color) && color == 0xFF4D6D);
    for (const char *text : {"", "0x", "0x1234567", "0xZZZZZZ", "-1"}) assert(!parse_color(text, color));
    assert(!parse_color(nullptr, color));
    int32_t value = 123, low = 123, high = 456;
    assert(scale_value(0, 100, value) && value == 0);
    assert(!scale_value(1, 0, value));
    assert(!scale_value(std::numeric_limits<double>::infinity(), 100, value));
    assert(!scale_value(std::numeric_limits<double>::quiet_NaN(), 100, value));
    assert(!scale_value(1e300, 100, value));
    assert(!scale_value(1e6 + 1, 1, value));
    assert(make_range(0.9, 0.1, 100, low, high) && low == 10 && high == 90);
    assert(!make_range(1, 1, 100, low, high));
    int32_t points[] = {LV_CHART_POINT_NONE, 0, -25, LV_CHART_POINT_NONE, 14};
    assert(find_range(points, 5, LV_CHART_POINT_NONE, low, high) && low == -25 && high == 14);
    assert(!find_range(points, 1, LV_CHART_POINT_NONE, low, high));

    config(R"({"config":"test","chart_bg":"0x212121","chart_series":[{"name":"value","min":0.9,"max":0.1,"scale":100,"fmt":"%.2f","color":"0x00FF00"}]})");
    assert(chart_series_prop.bgcolor == 0x212121 && chart_series_prop.series_count == 1);
    assert(chart_series_prop.sername[0] == "value" && chart_series_prop.value_fmt[0] == "%.2f");
    assert(chart_series_prop.palette[0] == 0x00FF00 && charts[0].low == 10 && charts[0].high == 90);
    config(R"({"config":"bad","chart_bg":123,"chart_series":[{"name":"value","scale":0,"fmt":"%s","color":42,"min":"bad","max":2}]})");
    assert(chart_series_prop.bgcolor == 0x212121);
    assert(chart_ranges[0].value_scale == 100 && !chart_ranges[0].is_initialized);
    assert(chart_series_prop.value_fmt[0] == "%.1f" && chart_series_prop.palette[0] == chart_palette[0]);
    config(R"({"config":"duplicates","chart_series":[{"name":"a"},{"name":"a"},{"name":42},{"name":"b"}]})");
    assert(chart_series_prop.series_count == 2 && chart_series_prop.sername[1] == "b");
    chart_range automatic;
    ui_update_chart_range(&charts[0], &automatic, 0);
    assert(automatic.is_initialized && automatic.min == 0 && automatic.max == 0);
    ui_update_chart_range(&charts[0], &automatic, -7);
    assert(automatic.min == -7 && automatic.max == 0);

    JsonDocument data;
    assert(!deserializeJson(data, R"({"zero":0,"bad":"42","flag":false,"label":"sensor"})"));
    assert(data["zero"].is<double>() && !data["bad"].is<double>());
    assert(!data["missing"].is<double>() && !data["flag"].is<double>());
}

static void check_startup()
{
    lv_timer_t timer;
    ui_imgSplash = reinterpret_cast<void *>(1);
    lineCount = now_ms = ui_startup_ms = 0;
    ui_editing_enabled = false;
    ui_startup_tick(&timer);
    assert(deleted_splashes == 0 && deleted_timers == 0);
    lineCount = 1;
    ui_startup_tick(&timer);
    assert(ui_imgSplash == nullptr && deleted_splashes == 1 && !ui_editing_enabled);
    now_ms = 5000;
    ui_startup_tick(&timer);
    assert(ui_editing_enabled && deleted_splashes == 1 && deleted_timers == 1);
    ui_imgSplash = reinterpret_cast<void *>(1);
    lineCount = 0;
    ui_editing_enabled = false;
    ui_startup_tick(&timer);
    assert(ui_editing_enabled && ui_imgSplash != nullptr && deleted_timers == 1);
    lineCount = 1;
    ui_startup_tick(&timer);
    assert(deleted_splashes == 2 && deleted_timers == 2);
}

struct FolderFS {
    int collisions = 0, exists_calls = 0, mkdir_calls = 0;
    bool mkdir_ok = true;
    bool exists(const char *) { return exists_calls++ < collisions; }
    bool mkdir(const char *) { ++mkdir_calls; return mkdir_ok; }
};

static void check_storage()
{
    char folder[10];
    unsigned id = 0;
    FolderFS last_attempt;
    last_attempt.collisions = 9;
    assert(create_recording_folder(last_attempt, folder, sizeof(folder), 10, [&]() { return ++id; }));
    assert(last_attempt.exists_calls == 10 && last_attempt.mkdir_calls == 1);
    FolderFS full;
    full.mkdir_ok = false;
    assert(!create_recording_folder(full, folder, sizeof(folder), 10, []() { return 1; }));
    assert(full.mkdir_calls == 10);
    FolderFS collisions;
    collisions.collisions = 100;
    assert(!create_recording_folder(collisions, folder, sizeof(folder), 10, []() { return 1; }));
    assert(collisions.mkdir_calls == 0);
    assert(!create_recording_folder(full, folder, 3, 10, []() { return 1; }));

    bandrate_index = 3;
    preferences.written = 0;
    save_bandrate_to_nvs();
    assert(bandrate_index_in_nvs == 7 && bandrate_index == 3);
    preferences.written = 1;
    save_bandrate_to_nvs();
    assert(bandrate_index_in_nvs == 3 && bandrate_index == 3);
    const int writes = preferences.writes;
    bandrate_index = 2;
    preferences.open_ok = false;
    save_bandrate_to_nvs();
    assert(bandrate_index_in_nvs == 3 && preferences.writes == writes);

    movAvg3 average;
    assert(average.result() == 0);
    average.push(UINT32_MAX); average.push(UINT32_MAX); average.push(UINT32_MAX);
    assert(average.result() == UINT32_MAX);
}

int main()
{
    check_chart_inputs();
    check_startup();
    check_storage();
    std::cout << "PASS: real chart parsing, format/color/numeric validation, startup lifecycle, directory retries, NVS failures and averaging\n";
}
