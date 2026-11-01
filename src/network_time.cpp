#include "network_time.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <sys/time.h>
#include <esp_wifi.h>
#include <esp_smartconfig.h>
#include "sdcard.h"
#include <cstring>

// All radio operations run in one low-priority task; callbacks only publish state.
static std::atomic<NetworkState> state{NetworkState::Unconfigured};
static std::atomic<const char *> failure_text{"WIFI?"};
static std::atomic<uint16_t> disconnect_reason{0};
static portMUX_TYPE clock_mutex = portMUX_INITIALIZER_UNLOCKED;
static bool clock_ready = false;
static int64_t clock_epoch_us = 0;
static uint64_t clock_monotonic_us = 0;
static bool ntp_started = false;
static std::atomic<bool> time_sync_pending_log{false};
static uint32_t connect_started_ms = 0;
static uint32_t last_retry_ms = 0;
static std::atomic<bool> provisioning_requested{false}, credentials_received{false}, ack_received{false};
static bool provisioning = false;
static uint32_t provisioning_started_ms = 0, provision_connected_ms = 0;

void network_request_provisioning() { provisioning_requested.store(true); }


static void on_time_sync(struct timeval *tv)
{
    if (tv->tv_sec < 1704067200) return;
    const uint64_t monotonic = esp_timer_get_time();
    portENTER_CRITICAL(&clock_mutex);
    clock_epoch_us = int64_t(tv->tv_sec) * 1000000 + tv->tv_usec;
    clock_monotonic_us = monotonic;
    clock_ready = true;
    portEXIT_CRITICAL(&clock_mutex);
    // SNTP invokes this on the TCP/IP task. Keep formatting off its stack.
    time_sync_pending_log.store(true);
}

int64_t network_epoch_us(uint64_t monotonic_us)
{
    portENTER_CRITICAL(&clock_mutex);
    const bool ready = clock_ready;
    const int64_t epoch = clock_epoch_us;
    const uint64_t base = clock_monotonic_us;
    portEXIT_CRITICAL(&clock_mutex);
    return ready ? epoch + int64_t(monotonic_us) - int64_t(base) : 0;
}

NetworkState network_state() { return state.load(); }
const char *network_status_text()
{
    switch (network_state())
    {
    case NetworkState::Provisioning: return "WIFI SETUP";
    case NetworkState::Connecting: return "WIFI...";
    case NetworkState::Online: return "WIFI";
    case NetworkState::Failed: return failure_text.load();
    default: return "WIFI CFG!";
    }
}

static void network_fail(const char *message)
{
    failure_text.store(message);
    state.store(NetworkState::Failed);
}

static bool initialize_station()
{
    if (WiFi.mode(WIFI_STA)) return true;
    network_fail("WIFI ERROR");
    log_e("WiFi STA initialization failed");
    return false;
}

static void start_connection(uint32_t now)
{
    connect_started_ms = last_retry_ms = now;
    WiFi.setAutoReconnect(true);
    state.store(NetworkState::Connecting);
    WiFi.begin(); // Load credentials saved by SmartConfig in Wi-Fi NVS.
}

static void stop_provisioning()
{
    if (provisioning)
    {
        log_i("SmartConfig stopping: ack=%d, internal_heap=%lu, stack_free=%lu",
              int(ack_received.load()),
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
        // WiFiGeneric stops the SDK before publishing SEND_ACK_DONE.
        if (!ack_received.load())
        {
            const esp_err_t result = esp_smartconfig_stop();
            log_i("SmartConfig stop returned: %d", int(result));
        }
        log_i("SmartConfig stopped");
    }
    provisioning = false;
    credentials_received.store(false);
    ack_received.store(false);
}

static void start_provisioning(uint32_t now)
{
    stop_provisioning();
    if (!initialize_station()) return;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(); // Preserve previously saved credentials until replacement arrives.
    smartconfig_start_config_t config = SMARTCONFIG_START_CONFIG_DEFAULT();
    if (esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS) != ESP_OK ||
        esp_smartconfig_start(&config) != ESP_OK)
    {
        network_fail("SETUP ERROR");
        log_e("SmartConfig start failed");
        return;
    }
    provisioning = true;
    provisioning_started_ms = now;
    provision_connected_ms = 0;
    state.store(NetworkState::Provisioning);
    sdcard_pairing_hint();
    log_i("SmartConfig started: ESPTouch/AirKiss, timeout=180s");
}

static void network_poll(uint32_t now)
{
    if (time_sync_pending_log.exchange(false))
        log_i("NTP time synchronized; network task stack_free=%lu",
              (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
    if (provisioning_requested.exchange(false)) start_provisioning(now);
    if (provisioning && credentials_received.exchange(false))
    {
        // Arduino's WiFiGeneric SC handler applies and persists phone credentials
        // before dispatching this event. Do not connect a second time here.
        state.store(NetworkState::Connecting);
        log_i("SmartConfig credentials received; connecting");
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (provisioning)
    {
        // Keep SmartConfig alive for the phone's acknowledgement, not just GOT_IP.
        if (connected && provision_connected_ms == 0) provision_connected_ms = now;
        if ((connected && (ack_received.load() || uint32_t(now - provision_connected_ms) >= 15000)) ||
            uint32_t(now - provisioning_started_ms) >= 180000)
        {
            stop_provisioning();
            WiFi.setAutoReconnect(true);
            if (!connected)
            {
                network_fail("SETUP TIMEOUT");
                last_retry_ms = now;
                return;
            }
        }
        else if (!connected) return;
    }
    if (connected)
    {
        if (network_state() != NetworkState::Online)
            log_i("WiFi online: internal_heap=%lu, stack_free=%lu",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
        state.store(NetworkState::Online);
        if (!ntp_started)
        {
            log_i("NTP initialization begin");
            sntp_set_time_sync_notification_cb(on_time_sync);
            configTzTime("CST-8", "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
            ntp_started = true;
            log_i("NTP initialization complete");
        }
        return;
    }
    if (network_state() == NetworkState::Online)
    {
        state.store(NetworkState::Connecting);
        connect_started_ms = last_retry_ms = now;
    }
    if (network_state() == NetworkState::Connecting && uint32_t(now - connect_started_ms) >= 30000)
    {
        network_fail("JOIN FAILED");
        log_w("WiFi connection timed out, disconnect_reason=%u", unsigned(disconnect_reason.load()));
    }
    if (uint32_t(now - last_retry_ms) >= 60000)
    {
        last_retry_ms = now;
        if (!initialize_station()) return;
        wifi_config_t saved{};
        if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0])
            start_connection(now);
    }
}

static void network_begin(uint32_t now)
{
    WiFi.persistent(true); // Phone-supplied credentials survive reboot.
    last_retry_ms = now;
    if (!initialize_station()) return;
    wifi_config_t saved{};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0])
        start_connection(now);
    else
        start_provisioning(now);
}

static void network_task(void *)
{
    network_begin(millis());
    for (;;)
    {
        network_poll(millis());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void network_time_init()
{
    WiFi.onEvent([](arduino_event_t *event) {
        // Publish events only; radio calls and logs belong to network_task.
        if (event->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            disconnect_reason.store(event->event_info.wifi_sta_disconnected.reason);
        else if (event->event_id == ARDUINO_EVENT_SC_GOT_SSID_PSWD)
        {
            credentials_received.store(true);
        }
        else if (event->event_id == ARDUINO_EVENT_SC_SEND_ACK_DONE)
            ack_received.store(true);
    });
    if (xTaskCreatePinnedToCore(network_task, "network time", 6144, nullptr, 1, nullptr, 0) != pdPASS)
    {
        network_fail("WIFI MEM!");
        log_e("WiFi task allocation failed");
    }
}
