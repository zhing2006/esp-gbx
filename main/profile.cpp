#include "console.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "driver/gptimer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gbx/app.h"
#include "gbx/display.h"
#include "gbx/executable.h"
#include "gbx/gb/system.h"
#include "gbx/gba/ppu.h"
#include "soc/cache_reg.h"
#include "soc/soc.h"

extern "C" int _text_start, _text_end;

namespace gbx::console {
namespace {

constexpr const char* kTag = "profile";

// The profiling build's commands, on top of console.cpp's:
//   bench [@]<skip> <n>
//                     times the GBA frames from <skip> to <skip> + <n> after
//                     the next slot load, or with @ of the running game,
//                     see app::bench_slot()
//   tasks <ms>        each task's share of a core over <ms>, in percent
//   stalls <s>        for <s> seconds, at each wait of the CPU core for the
//                     GBA renderer, each task's share of its core over
//                     about 140 ms before and the latest frames' drawing
//   prof <core> <ms>  samples the core's program counter every 197 us and
//                     prints the counts per 16 bytes in PROF lines, which
//                     tools/profile_report.py reads
//   cache <ms>        the cache controller's access counters over <ms>:
//                     hits, misses, conflicts and next-level reads (and
//                     writes for data) of each core's L1 instruction and
//                     data bus, and of the L2 for the same buses
//   cacheprobe        reads memory at known strides on core 1 and logs the
//                     cycles a read and those counters, which tells what
//                     they count
//   pstat <ms>        time the present task spends per second, by part
//   pstat reset|read  delimit a window without blocking console input;
//                     read without a reset first includes startup drawing
//   record int|jit [gbc] <file> <ROM path>
//                     loads the ROM, in a GBC with gbc, and records the
//                     keys of each frame for <file> on the card, see
//                     app::record_game()
//   record mark       a replay times its frames from here on
//   record stop       writes the recording
//   play int|jit <file>
//                     replays a recording; at its end it logs the timing
//                     of its frames from the mark and a state hash
//   heap [dump]       each internal RAM heap region's free bytes and largest
//                     free block; with dump, every block of the regions code
//                     may run from
// The status of "stat" adds a line on the slowest frame and the JIT.

void log_tasks(uint32_t ms) {
    constexpr UBaseType_t kMax = 40;
    // In PSRAM: internal RAM goes to the emulator.
    static auto* const before = static_cast<TaskStatus_t*>(heap_caps_malloc(2 * kMax * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    TaskStatus_t* const after = before + kMax;
    const UBaseType_t first = uxTaskGetSystemState(before, kMax, nullptr);
    const int64_t start = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(ms));
    const UBaseType_t last = uxTaskGetSystemState(after, kMax, nullptr);
    const double span = static_cast<double>(esp_timer_get_time() - start);
    char line[600];
    int used = std::snprintf(line, sizeof line, "tasks %lu ms:", static_cast<unsigned long>(ms));
    for (UBaseType_t i = 0; i < last; ++i) {
        for (UBaseType_t j = 0; j < first; ++j) {
            if (before[j].xHandle != after[i].xHandle) continue;
            const double percent = (after[i].ulRunTimeCounter - before[j].ulRunTimeCounter) * 100.0 / span;
            if (percent >= 0.05 && used < static_cast<int>(sizeof line) - 40)
                used += std::snprintf(line + used, sizeof line - used, " %s %.1f", after[i].pcTaskName, percent);
        }
    }
    ESP_LOGW(kTag, "%s", line);
}

// The stall watch: every 20 ms the tasks' run times, and at a new wait of
// the CPU core for the renderer, each task's share of its core since the
// oldest of the last 8 and the latest frames drawn. A wait within that
// span of the last one logged is not logged again, as logging takes
// core 0 too.
void stall_task(void* argument) {
    const auto seconds = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument));
    constexpr UBaseType_t kMax = 40;
    constexpr unsigned kSnapshots = 8, kPeriodMs = 20, kFrames = 8;
    // In PSRAM: internal RAM goes to the emulator.
    auto* const tasks = static_cast<TaskStatus_t*>(heap_caps_malloc(kSnapshots * kMax * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    UBaseType_t counts[kSnapshots];
    int64_t times[kSnapshots];
    uint32_t waits = gb::offload_counters.waits[0];
    int64_t logged = 0;
    const int64_t end = esp_timer_get_time() + int64_t{seconds} * 1'000'000;
    for (unsigned n = 0; tasks && esp_timer_get_time() < end; ++n, vTaskDelay(pdMS_TO_TICKS(kPeriodMs))) {
        const unsigned at = n % kSnapshots, oldest = (n + 1) % kSnapshots;
        counts[at] = uxTaskGetSystemState(tasks + at * kMax, kMax, nullptr);
        times[at] = esp_timer_get_time();
        const uint32_t seen = gb::offload_counters.waits[0];
        if (seen == waits) continue;
        waits = seen;
        if (n + 1 < kSnapshots || logged > times[oldest]) continue;
        logged = times[at];
        const TaskStatus_t* const before = tasks + oldest * kMax;
        const TaskStatus_t* const after = tasks + at * kMax;
        const double span = static_cast<double>(times[at] - times[oldest]);
        char line[600];
        int used = std::snprintf(line, sizeof line, "stalls: a wait at %lld us; over %.0f ms, task/priority %%:", times[at],
                                 span / 1000);
        for (UBaseType_t i = 0; i < counts[at]; ++i) {
            for (UBaseType_t j = 0; j < counts[oldest]; ++j) {
                if (before[j].xHandle != after[i].xHandle) continue;
                const double percent = (after[i].ulRunTimeCounter - before[j].ulRunTimeCounter) * 100.0 / span;
                if (percent >= 1 && used < static_cast<int>(sizeof line) - 40)
                    used += std::snprintf(line + used, sizeof line - used, " %s/%u %.0f", after[i].pcTaskName,
                                          static_cast<unsigned>(after[i].uxCurrentPriority), percent);
            }
        }
        ESP_LOGW(kTag, "%s", line);
        const auto& counters = gba::render_counters;
        const uint32_t drawn = counters.recent_count.load(std::memory_order_acquire);
        for (uint32_t k = drawn - std::min<uint32_t>(drawn, kFrames); k != drawn; ++k) {
            const auto& frame = counters.recent[k % gba::kRecentFrames];
            used = std::snprintf(line, sizeof line, "stalls: frame %llu drawn in %lu us; parts (k instructions):",
                                 static_cast<unsigned long long>(frame.frame), static_cast<unsigned long>(frame.draw_us));
            for (const uint32_t retired : frame.part_retired)
                used += std::snprintf(line + used, sizeof line - used, " %lu", static_cast<unsigned long>(retired / 1000));
            ESP_LOGW(kTag, "%s", line);
        }
    }
    heap_caps_free(tasks);
    ESP_LOGW(kTag, "stalls: done");
    vTaskDelete(nullptr);
}

void log_cache(uint32_t ms) {
    // Per bus: core 0 and 1's instruction (IBUS0, IBUS1) and data (DBUS0,
    // DBUS1) buses, as enable bits; their clear bits are 16 higher.
    constexpr uint32_t kL1 = CACHE_L1_IBUS0_CNT_ENA | CACHE_L1_IBUS1_CNT_ENA | CACHE_L1_DBUS0_CNT_ENA |
                             CACHE_L1_DBUS1_CNT_ENA,
                       kL2 = CACHE_L2_IBUS0_CNT_ENA | CACHE_L2_IBUS1_CNT_ENA | CACHE_L2_DBUS0_CNT_ENA |
                             CACHE_L2_DBUS1_CNT_ENA;
    REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, kL1 << 16);
    REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, kL2 << 16);
    REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, kL1);
    REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, kL2);
    vTaskDelay(pdMS_TO_TICKS(ms));
    REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, 0);
    REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, 0);
    const auto bus = [](const char* level, const char* name, uint32_t base, unsigned words) {
        uint32_t v[5]{};
        for (unsigned i = 0; i < words; ++i) v[i] = REG_READ(base + 4 * i);
        ESP_LOGW(kTag, "cache %s %s: hit %lu miss %lu conflict %lu next read %lu write %lu", level, name,
                 static_cast<unsigned long>(v[0]), static_cast<unsigned long>(v[1]), static_cast<unsigned long>(v[2]),
                 static_cast<unsigned long>(v[3]), static_cast<unsigned long>(v[4]));
    };
    bus("L1", "ibus0", CACHE_L1_IBUS0_ACS_HIT_CNT_REG, 4);
    bus("L1", "ibus1", CACHE_L1_IBUS1_ACS_HIT_CNT_REG, 4);
    bus("L1", "dbus0", CACHE_L1_DBUS0_ACS_HIT_CNT_REG, 5);
    bus("L1", "dbus1", CACHE_L1_DBUS1_ACS_HIT_CNT_REG, 5);
    bus("L2", "ibus0", CACHE_L2_IBUS0_ACS_HIT_CNT_REG, 4);
    bus("L2", "ibus1", CACHE_L2_IBUS1_ACS_HIT_CNT_REG, 4);
    bus("L2", "dbus0", CACHE_L2_DBUS0_ACS_HIT_CNT_REG, 5);
    bus("L2", "dbus1", CACHE_L2_DBUS1_ACS_HIT_CNT_REG, 5);
    ESP_LOGW(kTag, "cache %lu ms done", static_cast<unsigned long>(ms));
}

// What the cache counters count: reads words at known strides on core 1
// and logs mcycle per read with the counts (docs/esp32p4-performance.md).
struct CacheProbe {
    const char* name;
    volatile uint32_t* data;
    size_t words, stride, count;
    uint32_t cycles;
    SemaphoreHandle_t done;
};

void probe_task(void* context) {
    auto& p = *static_cast<CacheProbe*>(context);
    uint32_t sum = 0, start, end;
    size_t index = 0;
    const size_t step = p.stride / 4;
    asm volatile("csrr %0, mcycle" : "=r"(start));
    for (size_t i = 0; i < p.count; ++i) {
        sum += p.data[index];
        index += step;
        if (index >= p.words) index -= p.words;
    }
    asm volatile("csrr %0, mcycle" : "=r"(end));
    p.cycles = end - start + (sum & 0);
    xSemaphoreGive(p.done);
    vTaskDelete(nullptr);
}

void cache_probe() {
    constexpr uint32_t kL1 = CACHE_L1_DBUS0_CNT_ENA | CACHE_L1_DBUS1_CNT_ENA | CACHE_L1_IBUS1_CNT_ENA,
                       kL2 = CACHE_L2_DBUS0_CNT_ENA | CACHE_L2_DBUS1_CNT_ENA | CACHE_L2_IBUS1_CNT_ENA;
    auto* small = static_cast<uint32_t*>(heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    auto* large = static_cast<uint32_t*>(heap_caps_malloc(128 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    auto* psram = static_cast<uint32_t*>(heap_caps_malloc(4 << 20, MALLOC_CAP_SPIRAM));
    if (!small || !large || !psram) {
        ESP_LOGW(kTag, "cacheprobe: no memory %p %p %p", small, large, psram);
    } else {
        CacheProbe probes[] = {
            {"internal 8K seq", small, 2048, 4, 1000000, 0, nullptr},
            {"internal 128K stride 64", large, 32768, 64, 1000000, 0, nullptr},
            {"psram 32K stride 64", psram, 8192, 64, 1000000, 0, nullptr},
            {"psram 4M stride 64", psram, 1 << 20, 64, 1000000, 0, nullptr},
            {"psram 4M stride 4", psram, 1 << 20, 4, 1000000, 0, nullptr},
        };
        for (auto& p : probes) {
            p.done = xSemaphoreCreateBinary();
            REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, kL1 << 16);
            REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, kL2 << 16);
            REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, kL1);
            REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, kL2);
            xTaskCreatePinnedToCore(probe_task, "probe", 4096, &p, 10, nullptr, 1);
            xSemaphoreTake(p.done, portMAX_DELAY);
            REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, 0);
            REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, 0);
            vSemaphoreDelete(p.done);
            const auto r = [](uint32_t reg) { return static_cast<unsigned long>(REG_READ(reg)); };
            ESP_LOGW(kTag, "probe %s: %.1f cycles a read; L1 d1 hit %lu miss %lu conf %lu rd %lu wr %lu; L2 d1 hit %lu miss %lu rd %lu; L1 i1 hit %lu miss %lu rd %lu",
                     p.name, double(p.cycles) / p.count, r(CACHE_L1_DBUS1_ACS_HIT_CNT_REG),
                     r(CACHE_L1_DBUS1_ACS_MISS_CNT_REG), r(CACHE_L1_DBUS1_ACS_CONFLICT_CNT_REG),
                     r(CACHE_L1_DBUS1_ACS_NXTLVL_RD_CNT_REG), r(CACHE_L1_DBUS1_ACS_NXTLVL_WR_CNT_REG),
                     r(CACHE_L2_DBUS1_ACS_HIT_CNT_REG), r(CACHE_L2_DBUS1_ACS_MISS_CNT_REG),
                     r(CACHE_L2_DBUS1_ACS_NXTLVL_RD_CNT_REG), r(CACHE_L1_IBUS1_ACS_HIT_CNT_REG),
                     r(CACHE_L1_IBUS1_ACS_MISS_CNT_REG), r(CACHE_L1_IBUS1_ACS_NXTLVL_RD_CNT_REG));
        }
    }
    heap_caps_free(small);
    heap_caps_free(large);
    heap_caps_free(psram);
    ESP_LOGW(kTag, "cacheprobe done");
}

// The method of tests/device/profiler.cpp: the timer's interrupt runs on the
// core that set it up and reads the interrupted program counter.
namespace sampler {
constexpr unsigned kShift = 4;
uint32_t* text = nullptr;
uint32_t* ram = nullptr;
// The chip's mask ROM, which holds library and cache functions.
uint32_t* rom = nullptr;
constexpr size_t kRomBytes = SOC_IROM_MASK_HIGH - SOC_IROM_MASK_LOW;
// Generated code outside internal RAM, such as the JIT's second level.
constexpr size_t kCodeRegions = 4;
platform::ExecutableMemory code[kCodeRegions];
uint32_t* code_samples[kCodeRegions]{};
size_t code_count = 0;
uint32_t samples = 0, other = 0;
uintptr_t text_base() { return reinterpret_cast<uintptr_t>(&_text_start); }
size_t text_bytes() { return reinterpret_cast<uintptr_t>(&_text_end) - text_base(); }

bool on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    uint32_t pc;
    asm volatile("csrr %0, mepc" : "=r"(pc));
    ++samples;
    if (pc - text_base() < text_bytes()) ++text[(pc - text_base()) >> kShift];
    else if (pc - SOC_IRAM_LOW < SOC_IRAM_HIGH - SOC_IRAM_LOW) ++ram[(pc - SOC_IRAM_LOW) >> kShift];
    else if (pc - SOC_IROM_MASK_LOW < kRomBytes) ++rom[(pc - SOC_IROM_MASK_LOW) >> kShift];
    else {
        for (size_t i = 0; i < code_count; ++i) {
            const uintptr_t offset = pc - reinterpret_cast<uintptr_t>(code[i].words);
            if (offset < code[i].size_bytes) { ++code_samples[i][offset >> kShift]; return false; }
        }
        ++other;
    }
    return false;
}

struct Job {
    uint32_t ms;
    SemaphoreHandle_t done;
};

void run(void* context) {
    auto& job = *static_cast<Job*>(context);
    gptimer_handle_t timer = nullptr;
    gptimer_config_t config{};
    config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    config.direction = GPTIMER_COUNT_UP;
    config.resolution_hz = 1'000'000;
    gptimer_event_callbacks_t callbacks{};
    callbacks.on_alarm = on_alarm;
    gptimer_alarm_config_t alarm{};
    // Not a divisor of the emulator's periodic work. Each sample's interrupt
    // takes about 3.8 us on Sila P4: every 197 us costs the core about 2 %.
    alarm.alarm_count = 197;
    alarm.flags.auto_reload_on_alarm = true;
    if (gptimer_new_timer(&config, &timer) == ESP_OK) {
        if (gptimer_register_event_callbacks(timer, &callbacks, nullptr) == ESP_OK &&
            gptimer_set_alarm_action(timer, &alarm) == ESP_OK && gptimer_enable(timer) == ESP_OK) {
            if (gptimer_start(timer) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(job.ms));
                gptimer_stop(timer);
            }
            gptimer_disable(timer);
        }
        gptimer_del_timer(timer);
    }
    xSemaphoreGive(job.done);
    vTaskDelete(nullptr);
}

void profile(int core, uint32_t ms) {
    // The histograms live in PSRAM: internal RAM holds the JIT code pool.
    text = static_cast<uint32_t*>(heap_caps_calloc(text_bytes() >> kShift, 4, MALLOC_CAP_SPIRAM));
    ram = static_cast<uint32_t*>(heap_caps_calloc((SOC_IRAM_HIGH - SOC_IRAM_LOW) >> kShift, 4, MALLOC_CAP_SPIRAM));
    rom = static_cast<uint32_t*>(heap_caps_calloc(kRomBytes >> kShift, 4, MALLOC_CAP_SPIRAM));
    platform::ExecutableMemory found[kCodeRegions];
    code_count = 0;
    for (size_t i = 0, n = platform::external_executables(found); i < n; ++i) {
        // A region without a histogram counts as other.
        auto* histogram = static_cast<uint32_t*>(heap_caps_calloc(found[i].size_bytes >> kShift, 4, MALLOC_CAP_SPIRAM));
        if (!histogram) continue;
        code[code_count] = found[i];
        code_samples[code_count++] = histogram;
    }
    char regions[kCodeRegions * 18 + 2] = "-";
    for (size_t i = 0, used = 0; i < code_count; ++i)
        used += std::snprintf(regions + used, sizeof regions - used, "%s%x:%x", i ? "," : "",
                              static_cast<unsigned>(reinterpret_cast<uintptr_t>(code[i].words)),
                              static_cast<unsigned>(code[i].size_bytes));
    Job job{ms, xSemaphoreCreateBinary()};
    samples = other = 0;
    if (text && ram && rom && job.done &&
        xTaskCreatePinnedToCore(run, "gbx-sampler", 4096, &job, 10, nullptr, core) == pdPASS) {
        xSemaphoreTake(job.done, portMAX_DELAY);
        // Internal RAM code outside any symbol is the JIT's, as is the code
        // in the external regions.
        std::printf("PROF BEGIN core %d ms %lu samples %lu other %lu shift %u ram %x:%x rom %x:%x code %s\n", core,
                    static_cast<unsigned long>(ms), static_cast<unsigned long>(samples),
                    static_cast<unsigned long>(other), kShift, static_cast<unsigned>(SOC_IRAM_LOW),
                    static_cast<unsigned>(SOC_IRAM_HIGH - SOC_IRAM_LOW), static_cast<unsigned>(SOC_IROM_MASK_LOW),
                    static_cast<unsigned>(kRomBytes), regions);
        const auto put = [](const uint32_t* histogram, uintptr_t base, size_t bytes) {
            for (size_t i = 0; i < (bytes >> kShift); ++i)
                if (histogram[i]) std::printf("PROF %x %lx\n", static_cast<unsigned>(base + (i << kShift)),
                                              static_cast<unsigned long>(histogram[i]));
        };
        put(text, text_base(), text_bytes());
        put(ram, SOC_IRAM_LOW, SOC_IRAM_HIGH - SOC_IRAM_LOW);
        put(rom, SOC_IROM_MASK_LOW, kRomBytes);
        for (size_t i = 0; i < code_count; ++i)
            put(code_samples[i], reinterpret_cast<uintptr_t>(code[i].words), code[i].size_bytes);
        std::printf("PROF END\n");
        std::fflush(stdout);
    } else {
        ESP_LOGW(kTag, "prof: cannot sample");
    }
    if (job.done) vSemaphoreDelete(job.done);
    heap_caps_free(text);
    heap_caps_free(ram);
    heap_caps_free(rom);
    text = ram = rom = nullptr;
    for (size_t i = 0; i < code_count; ++i) heap_caps_free(code_samples[i]);
    code_count = 0;
}
}  // namespace sampler

int64_t present_start = 0;

void reset_present() {
    platform::display::take_present_times();
    platform::display::take_hardware_times();
    platform::display::take_frame_flow();
    present_start = esp_timer_get_time();
}

void read_present() {
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - present_start;
    const auto t = platform::display::take_present_times();
    const auto hw = platform::display::take_hardware_times();
    const auto flow = platform::display::take_frame_flow();
    present_start = now;
    const double s = elapsed > 0 ? elapsed / 1'000'000.0 : 1.0;
    ESP_LOGW(kTag, "pstat %lld us: loops %.1f/s games %.1f/s, ms/s: ui %.1f game %.1f flush %.1f; per game %.2f ms",
             static_cast<long long>(elapsed), t.loops / s, t.games / s, t.ui_us / 1000.0 / s, t.game_us / 1000.0 / s,
             t.flush_us / 1000.0 / s, t.games ? t.game_us / 1000.0 / t.games : 0.0);
    ESP_LOGW(kTag, "pstat total us: lvgl %lu lock %lu copy %lu fill %lu game %lu flush %lu present %lu scanout %lu",
             static_cast<unsigned long>(t.render_us), static_cast<unsigned long>(t.lock_us),
             static_cast<unsigned long>(t.ui_us), static_cast<unsigned long>(t.fill_us),
             static_cast<unsigned long>(t.game_us), static_cast<unsigned long>(t.flush_us),
             static_cast<unsigned long>(t.present_us), static_cast<unsigned long>(t.scanout_us));
    ESP_LOGW(kTag, "pstat counts: renders %lu rects %lu pixels %lu loops %lu games %lu display_bytes %u",
             static_cast<unsigned long>(t.renders), static_cast<unsigned long>(t.rects),
             static_cast<unsigned long>(t.pixels), static_cast<unsigned long>(t.loops),
             static_cast<unsigned long>(t.games), static_cast<unsigned>(t.buffer_bytes));
    const auto log_hardware = [](const char* name, const platform::display::HardwareOperationTimes& h) {
        ESP_LOGW(kTag, "pstat %s: submit_us %lu complete_us %lu count %lu max_us %lu", name,
                 static_cast<unsigned long>(h.submit_us), static_cast<unsigned long>(h.complete_us),
                 static_cast<unsigned long>(h.count), static_cast<unsigned long>(h.max_complete_us));
    };
    log_hardware("dma-copy", hw.copy);
    log_hardware("ppa-rotate", hw.rotate);
    log_hardware("ppa-fill", hw.fill);
    log_hardware("ppa-scale", hw.scale);
    log_hardware("dma-rows", hw.rows);
    ESP_LOGW(kTag, "pstat flow: published %lu overwritten %lu taken %lu scans %lu scan_hz %.3f lock_max_us %lu busy_max_us %lu",
             static_cast<unsigned long>(flow.published), static_cast<unsigned long>(flow.overwritten),
             static_cast<unsigned long>(flow.taken), static_cast<unsigned long>(flow.scans), flow.scans / s,
             static_cast<unsigned long>(flow.lock_max_us), static_cast<unsigned long>(flow.busy_max_us));
    const auto histogram = [](const char* name, const uint32_t (&buckets)[4]) {
        ESP_LOGW(kTag, "pstat %s: %lu %lu %lu %lu", name, static_cast<unsigned long>(buckets[0]),
                 static_cast<unsigned long>(buckets[1]), static_cast<unsigned long>(buckets[2]),
                 static_cast<unsigned long>(buckets[3]));
    };
    histogram("publish-interval-ms[<5,5-12,12-20,>=20]", flow.publish_interval);
    histogram("take-age-ms[<5,5-12,12-20,>=20]", flow.take_age);
    histogram("overwrite-age-ms[<5,5-12,12-20,>=20]", flow.overwrite_age);
    histogram("busy-scans[0,1,2,>=3]", flow.scan_cycles);
    constexpr const char* stages[] = {"notify", "dispatch", "lvgl-lock", "ui-copy", "fill", "nearest",
                                    "convert", "ppa-submit", "ppa-wait", "rows-wait", "submit", "scanout",
                                    "finish"};
    static_assert(std::size(stages) == static_cast<size_t>(platform::display::PresentStage::count));
    for (size_t i = 0; i < std::size(stages); ++i) {
        const auto& states = flow.overwrites[i];
        if (states[0] || states[1] || states[2] || states[3]) {
            ESP_LOGW(kTag, "pstat overwrite %s: running %lu ready %lu blocked %lu other %lu", stages[i],
                     static_cast<unsigned long>(states[0]), static_cast<unsigned long>(states[1]),
                     static_cast<unsigned long>(states[2]), static_cast<unsigned long>(states[3]));
        }
    }
    ESP_LOGW(kTag, "pstat heap: internal %u largest %u psram %u largest %u executable_largest %u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(platform::largest_executable()));
}

void log_present(uint32_t ms) {
    reset_present();
    vTaskDelay(pdMS_TO_TICKS(ms));
    read_present();
}

// "int" or "jit", then the next word, which `rest` points past.
bool backend_and_word(char* args, app::Backend& backend, char*& word, char*& rest) {
    if (std::strncmp(args, "int ", 4) != 0 && std::strncmp(args, "jit ", 4) != 0) return false;
    backend = args[0] == 'j' ? app::Backend::jit : app::Backend::interpreter;
    word = args + 4;
    while (*word == ' ') ++word;
    rest = std::strchr(word, ' ');
    if (rest) *rest++ = 0;
    else rest = word + std::strlen(word);
    return *word != 0;
}

void record_command(char* args) {
    if (std::strcmp(args, "mark") == 0) {
        app::mark_recording();
        return;
    }
    if (std::strcmp(args, "stop") == 0) {
        app::stop_recording();
        return;
    }
    app::Backend backend;
    char *file, *path;
    bool cgb = false;
    bool valid = backend_and_word(args, backend, file, path);
    if (valid && std::strcmp(file, "gbc") == 0) {
        cgb = true;
        file = path;
        while (*file == ' ') ++file;
        path = std::strchr(file, ' ');
        if (path) *path++ = 0;
        else path = file + std::strlen(file);
    }
    if (!valid || !*file || !*path) {
        ESP_LOGW(kTag, "record: use record int|jit [gbc] <file> <ROM path>, record mark or record stop");
        return;
    }
    app::record_game(path, backend, file, cgb);
}

void play_command(char* args) {
    app::Backend backend;
    char *file, *rest;
    if (!backend_and_word(args, backend, file, rest)) {
        ESP_LOGW(kTag, "play: use play int|jit <file>");
        return;
    }
    if (!app::replay_game(file, backend)) ESP_LOGW(kTag, "play: cannot read %s", file);
}

}  // namespace

bool profile_command(char* line) {
    if (std::strncmp(line, "record ", 7) == 0) {
        record_command(line + 7);
    } else if (std::strncmp(line, "play ", 5) == 0) {
        play_command(line + 5);
    } else if (std::strncmp(line, "bench ", 6) == 0) {
        const bool absolute = line[6] == '@';
        char* end = nullptr;
        const unsigned long skip = std::strtoul(line + 6 + absolute, &end, 10);
        app::bench_slot(static_cast<uint32_t>(skip), static_cast<uint32_t>(std::strtoul(end, nullptr, 10)), absolute);
        ESP_LOGW(kTag, "bench: frames %lu on %s", skip, absolute ? "of the game" : "after the next slot load");
    } else if (std::strncmp(line, "stalls ", 7) == 0) {
        // Above the tasks it watches, on the renderer's core.
        const unsigned long seconds = std::strtoul(line + 7, nullptr, 10);
        xTaskCreatePinnedToCore(stall_task, "stalls", 4096, reinterpret_cast<void*>(seconds), 10, nullptr, 0);
    } else if (std::strncmp(line, "tasks ", 6) == 0) {
        log_tasks(static_cast<uint32_t>(std::strtoul(line + 6, nullptr, 10)));
    } else if (std::strncmp(line, "prof ", 5) == 0) {
        char* end = nullptr;
        const long core = std::strtol(line + 5, &end, 10);
        sampler::profile(core == 1 ? 1 : 0, static_cast<uint32_t>(std::strtoul(end, nullptr, 10)));
    } else if (std::strcmp(line, "heap") == 0) {
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    } else if (std::strcmp(line, "heap dump") == 0) {
        heap_caps_dump(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC);
    } else if (std::strcmp(line, "cacheprobe") == 0) {
        cache_probe();
    } else if (std::strncmp(line, "cache ", 6) == 0) {
        log_cache(static_cast<uint32_t>(std::strtoul(line + 6, nullptr, 10)));
    } else if (std::strcmp(line, "pstat reset") == 0) {
        reset_present();
        ESP_LOGW(kTag, "pstat reset");
    } else if (std::strcmp(line, "pstat read") == 0) {
        read_present();
    } else if (std::strncmp(line, "pstat ", 6) == 0) {
        log_present(static_cast<uint32_t>(std::strtoul(line + 6, nullptr, 10)));
    } else {
        return false;
    }
    return true;
}

}  // namespace gbx::console
