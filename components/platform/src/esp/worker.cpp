#include "gbx/worker.h"

#include <new>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task_stack.h"

namespace gbx::platform {
Notification::~Notification() { if (state_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(state_)); }
bool Notification::initialize(unsigned capacity, unsigned initial) {
    if (state_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(state_));
    state_ = xSemaphoreCreateCounting(capacity, initial);
    return state_ != nullptr;
}
void Notification::wait() { xSemaphoreTake(static_cast<SemaphoreHandle_t>(state_), portMAX_DELAY); }
bool Notification::wait_for(uint32_t timeout_ms) {
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(state_), pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void Notification::signal() { xSemaphoreGive(static_cast<SemaphoreHandle_t>(state_)); }
namespace {
struct Task {
    void (*entry)(void*);
    void* context;
    Notification done;
    TaskHandle_t handle = nullptr;
    bool with_caps = false;  // to delete with vTaskDeleteWithCaps()
};
void trampoline(void* context) {
    auto& task = *static_cast<Task*>(context);
    task.entry(task.context);
    task.done.signal();
    vTaskSuspend(nullptr);
}
}
Worker::~Worker() { join(); }
bool Worker::start(void (*entry)(void*), void* context, unsigned core, const char* name, unsigned stack_bytes,
                   unsigned priority, StackMemory stack) {
    auto* task = new (std::nothrow) Task{entry, context, {}};
    if (!task) return false;
    if (!task->done.initialize(1)) { delete task; return false; }
    // A stack in PSRAM needs the cache on. With CONFIG_SPIRAM_XIP_FROM_PSRAM
    // flash reads leave it on; only erasing or writing flash while a region
    // is mapped turns it off, and the firmware does neither while it runs.
    // The TCB stays internal. Without PSRAM the stack goes internal. A
    // single core takes every worker.
    core %= portNUM_PROCESSORS;
    task->with_caps = stack == StackMemory::external &&
        xTaskCreatePinnedToCoreWithCaps(trampoline, name, stack_bytes, task, priority, &task->handle, core,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    if (!task->with_caps &&
        !create_internal_task(trampoline, name, stack_bytes, task, priority, &task->handle, core, &task->with_caps)) {
        delete task; return false;
    }
    state_ = task;
    return true;
}
void Worker::join() {
    if (!state_) return;
    auto* task = static_cast<Task*>(state_);
    task->done.wait();
    // The other core must leave xSemaphoreGive before its semaphore is freed.
    // Join a suspended task; self-deletion would not expose that boundary.
    while (eTaskGetState(task->handle) != eSuspended) vTaskDelay(1);
    if (task->with_caps) vTaskDeleteWithCaps(task->handle);
    else vTaskDelete(task->handle);
    delete task; state_ = nullptr;
}
unsigned current_core() { return xPortGetCoreID(); }
}  // namespace gbx::platform
