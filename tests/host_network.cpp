#include "network_time.h"
#include "hold_button.h"
#include "chart_timeline.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/time.h>

constexpr int WIFI_STA = 1, WL_CONNECTED = 3;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static unsigned network_log_calls = 0;
#define log_i(...) (++network_log_calls)
#define log_w(...) ((void)0)
#define log_e(...) ((void)0)
static bool connected = false, mode_ok = true, persistent = true;
static unsigned begins = 0, ntp_calls = 0;
constexpr int ESP_OK = 0, WIFI_IF_STA = 0, SC_TYPE_ESPTOUCH_AIRKISS = 2;
using esp_err_t = int;
struct wifi_config_t { struct { unsigned char ssid[32]{}; } sta; } saved_config;
struct smartconfig_start_config_t {};
#define SMARTCONFIG_START_CONFIG_DEFAULT() smartconfig_start_config_t{}
static bool setup_ok = true;
static unsigned setup_starts = 0, setup_stops = 0, hints = 0;
static int esp_wifi_get_config(int, wifi_config_t *out) { *out = saved_config; return ESP_OK; }
static int esp_smartconfig_set_type(int type) { assert(type == SC_TYPE_ESPTOUCH_AIRKISS); return ESP_OK; }
static int esp_smartconfig_start(smartconfig_start_config_t *) { ++setup_starts; return setup_ok ? ESP_OK : -1; }
static int esp_smartconfig_stop() { ++setup_stops; return ESP_OK; }
static void sdcard_pairing_hint() { ++hints; }
struct FakeWiFi {
    bool mode(int) { return mode_ok; }
    void persistent(bool enabled) { ::persistent = enabled; }
    void setAutoReconnect(bool) {}
    void disconnect() { connected = false; }
    void begin() { ++begins; }
    int status() { return connected ? WL_CONNECTED : 0; }
} WiFi;
static int64_t fake_us = 0;
static int64_t esp_timer_get_time() { return fake_us; }
static void sntp_set_time_sync_notification_cb(void (*)(timeval *)) {}
static void configTzTime(const char *, const char *, const char *, const char *) { ++ntp_calls; }
#include "network_functions.inc"

int main()
{
    network_begin(100);
    assert(persistent && begins == 0 && setup_starts == 1 && hints == 1);
    assert(network_state() == NetworkState::Provisioning);
    assert(network_epoch_us(0) == 0);
    credentials_received.store(true); // Framework already applied phone config.
    saved_config.sta.ssid[0] = 'x';
    network_poll(200);
    assert(network_state() == NetworkState::Connecting && begins == 0);
    connected = true; network_poll(300);
    assert(network_state() == NetworkState::Online && ntp_calls == 1 && provisioning);
    ack_received.store(true); network_poll(400);
    assert(!provisioning && setup_stops == 0); // Framework already stopped on ACK.
    timeval tv{1704067200, 0}; fake_us = 10000000;
    const auto before_sync_logs = network_log_calls;
    on_time_sync(&tv);
    assert(network_log_calls == before_sync_logs && time_sync_pending_log.load());
    network_poll(500);
    assert(network_log_calls == before_sync_logs + 1 && !time_sync_pending_log.load());
    assert(network_epoch_us(9000000) == 1704067199000000LL);
    connected = false; network_poll(70000);
    assert(network_state() == NetworkState::Connecting);
    assert(network_epoch_us(11000000) == 1704067201000000LL);
    network_poll(100000); assert(network_state() == NetworkState::Failed);
    network_poll(130000); assert(begins == 1);
    network_request_provisioning(); network_poll(140000);
    assert(provisioning && setup_starts == 2 && hints == 2);
    network_poll(319999); assert(provisioning);
    network_poll(320000);
    assert(!provisioning && strcmp(network_status_text(), "SETUP TIMEOUT") == 0);
    network_poll(380000); assert(begins == 2); // Saved network survives timeout.
    network_begin(390000); assert(begins == 3); // Reboot uses NVS, no provisioning.
    mode_ok = false; network_request_provisioning(); network_poll(400000);
    assert(network_state() == NetworkState::Failed && strcmp(network_status_text(), "WIFI ERROR") == 0);
    mode_ok = true; setup_ok = false;
    network_request_provisioning(); network_poll(410000);
    assert(!provisioning && strcmp(network_status_text(), "SETUP ERROR") == 0);
    setup_ok = true; network_request_provisioning(); network_poll(420000);
    connected = true; network_poll(420100);
    network_poll(435100); assert(!provisioning); // ACK lost: bounded cleanup.
    HoldButton button(5000);
    using E = HoldButton::Event;
    assert(button.update(true, 100) == E::None);
    assert(button.update(true, 135) == E::None);
    assert(button.update(true, 5099) == E::None);
    assert(button.update(true, 5100) == E::Long);
    assert(button.update(true, 10000) == E::None);
    assert(button.update(false, 11000) == E::None);
    assert(button.update(false, 11035) == E::None);
    button.update(true, 12000); button.update(true, 12035);
    button.update(false, 12100);
    assert(button.update(false, 12135) == E::Short);
    HoldButton wrap(5000);
    wrap.update(true, UINT32_MAX - 100); wrap.update(true, UINT32_MAX - 65);
    assert(wrap.update(true, 4899) == E::Long);
    HoldButton bounce(5000);
    bounce.update(true, 10); bounce.update(false, 20);
    assert(bounce.update(false, 100) == E::None);

    ChartTimeline<3> timeline;
    assert(timeline.empty());
    timeline.push(100, 1); timeline.push(350, 2); timeline.push(900, 3);
    assert(timeline.span_us() == 800 && timeline.latest_epoch_us == 3);
    timeline.push(2000, 4);
    assert(timeline.span_us() == 1650 && timeline.latest() == 2000);
    timeline.clear(); assert(timeline.empty() && timeline.latest_epoch_us == 0);
    char text[48];
    format_chart_span(text, sizeof(text), 3600000000ULL); assert(strcmp(text, "-1hr") == 0);
    format_chart_span(text, sizeof(text), 390000000); assert(strcmp(text, "-6.5min") == 0);
    format_chart_span(text, sizeof(text), 0); assert(strcmp(text, "-0s") == 0);
    std::cout << "PASS: first-boot SmartConfig, saved credentials, reconfigure/ACK/timeout/failure and reconnect clock; hold-button boundaries and irregular chart timeline\n";
}
