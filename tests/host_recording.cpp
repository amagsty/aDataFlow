#include "recording_helpers.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define log_i(...) ((void)0)
#define log_w(...) ((void)0)
#define log_e(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
constexpr int LOW = 0, HIGH = 1, PIN_LED_REC = 3, PIN_SD_CD = 12, SDMMC_FREQ_DEFAULT = 20, CARD_NONE = 0;
constexpr int FILE_APPEND = 1, CREATE_MAX_RETRY = 10, portMAX_DELAY = -1;
void *recording_mutex = reinterpret_cast<void *>(1);
int lock_depth = 0;
void xSemaphoreTake(void *, int) { assert(lock_depth == 0); ++lock_depth; }
void xSemaphoreGive(void *) { assert(lock_depth == 1); --lock_depth; }
struct EndIteration {};
static bool testing_led = false;
static int led_delay = 0;
static std::vector<unsigned> led_duties;
void vTaskDelay(int delay) { if (testing_led) led_delay += delay; else throw EndIteration{}; }
void digitalWrite(int pin, int duty) { assert(pin == PIN_LED_REC); led_duties.push_back(duty ? 255 : 0); }
void ledcWrite(int pin, unsigned duty) { assert(pin == PIN_LED_REC); led_duties.push_back(duty); }
static bool rec_led_pwm = true;
static constexpr uint8_t REC_LED_DUTY = 38;
uint32_t millis() { return 123456; }
bool card_inserted = true, batt_low = false, hold_power_on = false;
int digitalRead(int) { return card_inserted ? LOW : 1; }
unsigned next_folder = 0;
long random(int) { return next_folder++; }
std::atomic<bool> is_recording{false};
bool is_first_check = true;
std::atomic<bool> pending_toggle{false}, recording_transition{false};
std::atomic<uint8_t> led_blink_type{0};
char rand_folder_name[10] = {};

struct FileState { bool directory = false, closed = false, partial = false; int writes = 0; };
struct File {
    std::shared_ptr<FileState> state;
    explicit operator bool() const { return state && !state->closed; }
    bool isDirectory() { return bool(*this) && state->directory; }
    void close() { assert(lock_depth == 1); if (state) state->closed = true; }
    size_t print(const char *data) {
        assert(lock_depth == 1 && bool(*this)); ++state->writes;
        return std::strlen(data) - int(state->partial);
    }
};
File new_file_rx, new_file_tx;
struct Filesystem {
    bool mounted = false, mount_ok = true, mkdir_ok = true, fail_tx = false;
    int collisions = 0, checks = 0, mkdir_calls = 0, file_opens = 0;
    std::set<std::string> directories;
    std::vector<std::shared_ptr<FileState>> opened;
    bool begin(const char *, bool, bool, int) { mounted = mount_ok; return mounted; }
    int cardType() { return 1; }
    void end() { assert(lock_depth == 1); mounted = false; }
    bool exists(const char *path) { return ++checks <= collisions || directories.count(path); }
    bool mkdir(const char *path) { ++mkdir_calls; if (mkdir_ok) directories.insert(path); return mkdir_ok; }
    File open(const char *path, int mode = 0) {
        assert(lock_depth == 1);
        if (mode == FILE_APPEND) {
            assert(!is_recording); // Recording must not start until both files exist.
            ++file_opens;
            if (fail_tx && std::string(path).find("/TX_") != std::string::npos) return {};
        } else if (!directories.count(path)) return {};
        auto state = std::make_shared<FileState>();
        state->directory = mode != FILE_APPEND; opened.push_back(state);
        return {state};
    }
} SD_MMC;

#include "recording_functions.inc"

void reset()
{
    assert(lock_depth == 0);
    SD_MMC = Filesystem{};
    new_file_rx = {}; new_file_tx = {};
    is_recording = false; is_first_check = true;
    pending_toggle = false; recording_transition = false;
    card_inserted = true; batt_low = false; hold_power_on = false;
    next_folder = 0; led_blink_type = 0;
}

void toggle_once()
{
    sdcard_toggle_rec();
    try { task_rec_status(nullptr); } catch (EndIteration &) {}
    assert(!recording_transition.load() && !hold_power_on && lock_depth == 0);
}

int main()
{
    reset(); SD_MMC.collisions = 9; toggle_once();
    assert(is_recording && SD_MMC.file_opens == 2 && led_blink_type == 10);
    assert(SD_MMC.checks == 10 && SD_MMC.mkdir_calls == 1);
    toggle_once();
    assert(!is_recording && !SD_MMC.mounted);
    for (auto &file : SD_MMC.opened) assert(file->closed);

    reset(); SD_MMC.fail_tx = true; toggle_once();
    assert(!is_recording && !SD_MMC.mounted && led_blink_type == 2);
    assert(SD_MMC.opened.size() == 1 && SD_MMC.opened[0]->closed);

    reset(); SD_MMC.mkdir_ok = false; toggle_once();
    assert(!is_recording && !SD_MMC.mounted && SD_MMC.file_opens == 0);
    assert(SD_MMC.mkdir_calls == 10);
    reset(); SD_MMC.mount_ok = false; toggle_once();
    assert(!is_recording && SD_MMC.file_opens == 0);
    reset(); card_inserted = false; toggle_once();
    assert(!is_recording && led_blink_type == 1);

    reset(); toggle_once();
    new_file_rx.state->partial = true;
    char data[] = "test\n";
    rec(true, data);
    assert(!is_recording && !SD_MMC.mounted && !hold_power_on && lock_depth == 0);
    assert(new_file_rx.state->closed && new_file_tx.state->closed);

    reset(); toggle_once();
    recording_transition.store(true);
    rec(true, data);
    assert(new_file_rx.state->writes == 0);
    recording_transition.store(false);
    card_inserted = false;
    rec(false, data);
    assert(!is_recording && !SD_MMC.mounted && lock_depth == 0);
    testing_led = true;
    for (uint8_t command : {1, 2, 10, 99})
    {
        is_recording = command == 10;
        led_blink_type = command;
        led_duties.clear(); led_delay = 0;
        rec_led_tick();
        for (unsigned duty : led_duties) assert(duty == 0 || duty == 38);
        if (command == 1 || command == 2)
        {
            assert(led_duties.size() == 5);
            assert(led_duties[0] == 38 && led_duties[1] == 0);
            assert(led_duties[2] == 38 && led_duties[3] == 0);
            assert(led_delay == (command == 1 ? 600 : 300));
        }
        assert(led_duties.back() == (is_recording ? 38u : 0u));
    }
    std::cout << "PASS: actual SD start/stop/write flow, final retry success, failed second file, mkdir failure, missing card and partial write cleanup\n";
}
