#include "gbx/executable.h"

#include <array>
#include <atomic>
#include <limits>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#include "hal/cache_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_ESP_SYSTEM_MEMPROT
#error JIT executable allocation requires CONFIG_ESP_SYSTEM_MEMPROT disabled
#endif

namespace gbx::platform {

namespace {

// P4's internal L2MEM is cached through L1; both L1 I/D lines use this size.
// S31's ordinary internal SRAM is uncached, despite having external I/D caches.
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
constexpr size_t kAlignment = CONFIG_CACHE_L1_CACHE_LINE_SIZE;
#else
constexpr size_t kAlignment = alignof(uint32_t);
#endif
static_assert((kAlignment & (kAlignment - 1)) == 0);

// Code in PSRAM, on P4 only: with ESP_SYSTEM_MEMPROT off, its PMP entry
// allows execution on v3.x and none restricts it before v3.0, and PMA entry
// 13 makes the range executable (IDF components/esp_hw_support/port/esp32p4/
// cpu_region_protect.c). PSRAM goes through the same L1 caches, so it is
// published as internal memory is, see publish_executable().
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM
constexpr bool kExternalCode = true;
#else
constexpr bool kExternalCode = false;
#endif
#if CONFIG_GBX_JIT_CODE_PSRAM
constexpr bool kPoolExternal = true;
#else
constexpr bool kPoolExternal = false;
#endif
static_assert(kExternalCode || !kPoolExternal, "GBX_JIT_CODE_PSRAM needs PSRAM that can run code");

#ifdef GBX_PROFILER
// The external allocations, for external_executables(). The JIT's task
// allocates and frees them; a profiler on another task reads them.
constexpr size_t kTracked = 4;
std::array<std::atomic<uint32_t*>, kTracked> tracked_words{};
std::array<std::atomic<size_t>, kTracked> tracked_bytes{};

void track(uint32_t* words, size_t bytes) {
    for (size_t i = 0; i < kTracked; ++i) {
        if (tracked_words[i].load(std::memory_order_relaxed)) continue;
        tracked_bytes[i].store(bytes, std::memory_order_relaxed);
        tracked_words[i].store(words, std::memory_order_release);
        return;
    }
}

void untrack(const uint32_t* words) {
    for (auto& slot : tracked_words)
        if (slot.load(std::memory_order_relaxed) == words) { slot.store(nullptr, std::memory_order_release); return; }
}
#endif

}  // namespace

bool executable_available() {
    return true;
}

ExecutableMemory allocate_executable(size_t bytes, CodeMemory where) {
    if (!bytes || bytes > std::numeric_limits<size_t>::max() - (kAlignment - 1)) return {};
    bytes = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    if (where == CodeMemory::external || kPoolExternal) {
        if (!kExternalCode) return {};
        auto* data = static_cast<uint32_t*>(heap_caps_aligned_alloc(kAlignment, bytes, MALLOC_CAP_SPIRAM));
        if (!data) return {};
        const auto* last = reinterpret_cast<const uint8_t*>(data) + bytes - 1;
        if (!esp_ptr_external_ram(data) || !esp_ptr_external_ram(last)) {
            heap_caps_free(data);
            return {};
        }
#ifdef GBX_PROFILER
        track(data, bytes);
#endif
        return {data, bytes};
    }
    // IDF rejects EXEC combined with 8BIT or DMA. EXEC implies 32-bit access.
    auto* data = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        kAlignment, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC));
    if (!data) return {};
    // Do not silently accept RTC RAM fallback: only ordinary internal SRAM is
    // covered by this publication implementation and the initial JIT contract.
    const auto* last = reinterpret_cast<const uint8_t*>(data) + bytes - 1;
    if (!esp_ptr_in_iram(data) || !esp_ptr_in_iram(last)) {
        heap_caps_free(data);
        return {};
    }
    return {data, bytes};
}

size_t largest_executable() {
    if (kPoolExternal) return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC);
}

void free_executable(ExecutableMemory& memory) {
#ifdef GBX_PROFILER
    if (memory.words) untrack(memory.words);
#endif
    heap_caps_free(memory.words);
    memory = {};
}

bool publish_executable(const ExecutableMemory& memory, size_t offset, size_t bytes) {
    if (!memory.words || (offset % 4) || (bytes % 4) || offset > memory.size_bytes ||
        bytes > memory.size_bytes - offset) return false;
    if (!bytes) return true;
    asm volatile("fence iorw, iorw" ::: "memory");
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    const size_t first = offset & ~(kAlignment - 1);
    const size_t end = (offset + bytes + kAlignment - 1) & ~(kAlignment - 1);
    const auto address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(memory.words) + first);
    // The L1 operations esp_cache_msync() performs for internal memory, minus
    // its argument checks and its D-cache invalidation for an I-cache request:
    // written-back lines stay valid. Its lock keeps the other core's cache
    // operations out. PSRAM needs the same two: behind the L1 I- and
    // D-caches, P4 has one L2 cache for both (CACHE_MAP_L2_CACHE in IDF
    // hal/esp32p4/include/hal/cache_ll.h), so an instruction fetch that
    // misses L1 finds the written-back line there. The JIT backend checks
    // rewrite published code in both memories and run it again.
    esp_cache_sync_ops_enter_critical_section();
    cache_ll_l1_writeback_dcache_addr(CACHE_LL_ID_ALL, address, end - first);
    cache_ll_l1_invalidate_icache_addr(CACHE_LL_ID_ALL, address, end - first);
    esp_cache_sync_ops_exit_critical_section();
#endif
    // Zifencei synchronizes the local instruction stream, not remote execution.
    asm volatile("fence.i" ::: "memory");
    return true;
}

#ifdef GBX_PROFILER
size_t external_executables(std::span<ExecutableMemory> out) {
    size_t count = 0;
    for (size_t i = 0; i < kTracked && count < out.size(); ++i) {
        auto* words = tracked_words[i].load(std::memory_order_acquire);
        if (words) out[count++] = {words, tracked_bytes[i].load(std::memory_order_relaxed)};
    }
    return count;
}
#endif

}  // namespace gbx::platform
