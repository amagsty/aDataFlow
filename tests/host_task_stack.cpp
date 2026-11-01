#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskFunction_t = void (*)(void *);
using TaskHandle_t = void *;
static void worker(void *) {}
static uint32_t last_stack;
static const char *expected_name;
static TaskHandle_t handle;
static int argument;

extern "C" BaseType_t __real_xTaskCreatePinnedToCore(
    TaskFunction_t task, const char *name, uint32_t stack, void *parameter,
    UBaseType_t priority, TaskHandle_t *out, BaseType_t core)
{
    assert(task == worker && name == expected_name && parameter == &argument);
    assert(priority == 2 && out == &handle && core == -1);
    last_stack = stack;
    return 7;
}
#include "task_stack_wrapper.inc"

int main()
{
    for (const char *name : {"sc_ack_send_task", "sc_ack_send_tas", "uart_event_task", "network time", ""})
    {
        expected_name = name;
        for (const uint32_t bytes : {2048u, 6144u, 8192u})
        {
            assert(__wrap_xTaskCreatePinnedToCore(worker, name, bytes, &argument, 2, &handle, -1) == 7);
            assert(last_stack == ((std::strcmp(name, "sc_ack_send_task") == 0 && bytes < 6144) ? 6144 : bytes));
        }
    }
    expected_name = nullptr;
    assert(__wrap_xTaskCreatePinnedToCore(worker, nullptr, 2048, &argument, 2, &handle, -1) == 7);
    assert(last_stack == 2048);
    std::cout << "PASS: actual task wrapper expands only SmartConfig ACK stack, preserves all arguments and result\n";
}
