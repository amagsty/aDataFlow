#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

// ESP-IDF 5.5.5's prebuilt smartconfig_ack.c creates this task with 2048
// bytes. Device logs caught its stack canary firing during provisioning.
// No SDK setting exposes its stack size. Intercept only this named task;
// retain its function, parameters, priority, handle and core affinity.
extern "C" BaseType_t __real_xTaskCreatePinnedToCore(
    TaskFunction_t task, const char *name, uint32_t stack_bytes, void *parameter,
    UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);

extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(
    TaskFunction_t task, const char *name, uint32_t stack_bytes, void *parameter,
    UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    if (name != nullptr && std::strcmp(name, "sc_ack_send_task") == 0 && stack_bytes < 6144)
        stack_bytes = 6144;
    return __real_xTaskCreatePinnedToCore(task, name, stack_bytes, parameter,
                                        priority, handle, core);
}
