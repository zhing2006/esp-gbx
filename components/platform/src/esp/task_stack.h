#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace gbx::platform {

// Creates a task whose stack is internal, kept out of the block the JIT code
// pools need where the heap allows. On P4 before v3 the internal heap is two
// blocks of the same SRAM: IDF serves every internal allocation from the
// upper one, where the pools go, while it has room, and the lower one,
// IDF's retention memory, holds only what a retention request names
// (components/heap/port/esp32p4/memory_layout.c). A full lower block leaves
// the stack where IDF puts it. `with_caps`, when given, tells whether the
// task is to be deleted with vTaskDeleteWithCaps(). On a single core every
// task runs on core 0, whichever core it asks for.
inline bool create_internal_task(TaskFunction_t entry, const char* name, uint32_t stack_bytes, void* context,
                                 UBaseType_t priority, TaskHandle_t* handle, BaseType_t core,
                                 bool* with_caps = nullptr) {
    core %= portNUM_PROCESSORS;
    bool created = false;
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    created = xTaskCreatePinnedToCoreWithCaps(entry, name, stack_bytes, context, priority, handle, core,
                                              MALLOC_CAP_RETENTION | MALLOC_CAP_8BIT) == pdPASS;
#endif
    if (with_caps) *with_caps = created;
    return created || xTaskCreatePinnedToCore(entry, name, stack_bytes, context, priority, handle, core) == pdPASS;
}

}  // namespace gbx::platform
