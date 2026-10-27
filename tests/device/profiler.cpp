#include "profiler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "driver/gptimer.h"
#include "esp_heap_caps.h"
#include "soc/soc.h"
#include "transport.h"

extern "C" int _text_start, _text_end;

namespace gbx::test::profiler {
namespace {
// Not a divisor of the emulator's periodic work. Each sample's interrupt
// takes about 3.8 us on Sila P4, whatever the callback does: every 197 us
// costs the sampled core about 2 %, where 47 us cost 8 %.
constexpr uint32_t kPeriodUs = 197;
constexpr unsigned kShift = 3;  // 8-byte buckets.
// Generated code outside internal RAM, such as the JIT's 8 MiB second level,
// in 16-byte buckets to keep its histogram at 2 MiB.
constexpr unsigned kCodeShift = 4;
constexpr uintptr_t kRamBase = SOC_IRAM_LOW;
constexpr size_t kRamBytes = SOC_IRAM_HIGH - SOC_IRAM_LOW;
// The chip's mask ROM, which holds library and cache functions.
constexpr uintptr_t kRomBase = SOC_IROM_MASK_LOW;
constexpr size_t kRomBytes = SOC_IROM_MASK_HIGH - SOC_IROM_MASK_LOW;
// Every profile frame starts with this line; the runner keeps these frames
// apart from regular logs.
constexpr char kMarker[] = "gbx-profile\n";

uintptr_t text_base() { return reinterpret_cast<uintptr_t>(&_text_start); }
size_t text_bytes() { return reinterpret_cast<uintptr_t>(&_text_end) - text_base(); }

bool on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void* context) {
    uint32_t pc;
    asm volatile("csrr %0, mepc" : "=r"(pc));
    static_cast<Profile*>(context)->record(pc);
    return false;
}

const struct { const char* name; uint32_t gb::Sm83JitCounters::* field; } kCounters[]{
    {"execute", &gb::Sm83JitCounters::execute}, {"enter", &gb::Sm83JitCounters::enter},
    {"budget", &gb::Sm83JitCounters::budget}, {"cf_dynamic", &gb::Sm83JitCounters::cf_dynamic},
    {"cf_linked", &gb::Sm83JitCounters::cf_linked}, {"cf_none", &gb::Sm83JitCounters::cf_none},
    {"hit", &gb::Sm83JitCounters::hit}, {"mid", &gb::Sm83JitCounters::mid},
    {"compiles", &gb::Sm83JitCounters::compiles}, {"interpreted", &gb::Sm83JitCounters::interpreted},
    {"interpreted_instructions", &gb::Sm83JitCounters::interpreted_instructions},
    {"stale", &gb::Sm83JitCounters::stale},
    {"smc", &gb::Sm83JitCounters::smc}, {"deadline", &gb::Sm83JitCounters::deadline},
    {"stop_interrupt", &gb::Sm83JitCounters::stop_interrupt},
    {"stop_mapping", &gb::Sm83JitCounters::stop_mapping}, {"stop_until", &gb::Sm83JitCounters::stop_until},
    {"stop_frame", &gb::Sm83JitCounters::stop_frame}, {"stop_event", &gb::Sm83JitCounters::stop_event},
    {"group", &gb::Sm83JitCounters::group}, {"group_stop", &gb::Sm83JitCounters::group_stop},
    {"read", &gb::Sm83JitCounters::read}, {"write", &gb::Sm83JitCounters::write},
    {"code_write", &gb::Sm83JitCounters::code_write}, {"idle", &gb::Sm83JitCounters::idle}};
// Indexed by gb::JitExit.
constexpr const char* kExits[]{"limit", "control_flow", "suspended", "fallback", "event", "breakpoint", "refetch"};
}  // namespace

Profile::~Profile() {
    stop();
    heap_caps_free(text_);
    heap_caps_free(ram_);
    heap_caps_free(rom_);
    for (size_t i = 0; i < code_count_; ++i) heap_caps_free(code_samples_[i]);
}

void Profile::start(const gb::Sm83Jit* jit, const std::atomic<bool>* gate) {
    // The histograms live in PSRAM: internal RAM holds the JIT code pool.
    text_ = static_cast<uint32_t*>(heap_caps_calloc(text_bytes() >> kShift, 4, MALLOC_CAP_SPIRAM));
    ram_ = static_cast<uint32_t*>(heap_caps_calloc(kRamBytes >> kShift, 4, MALLOC_CAP_SPIRAM));
    rom_ = static_cast<uint32_t*>(heap_caps_calloc(kRomBytes >> kShift, 4, MALLOC_CAP_SPIRAM));
    if (!text_ || !ram_ || !rom_) {
        heap_caps_free(text_); heap_caps_free(ram_); heap_caps_free(rom_);
        text_ = ram_ = rom_ = nullptr;
        return;
    }
    platform::ExecutableMemory found[kCodeRegions];
    for (size_t i = 0, n = platform::external_executables(found); i < n; ++i) {
        // A region without a histogram counts as other.
        auto* histogram = static_cast<uint32_t*>(
            heap_caps_calloc(found[i].size_bytes >> kCodeShift, 4, MALLOC_CAP_SPIRAM));
        if (!histogram) continue;
        code_[code_count_] = found[i];
        code_samples_[code_count_++] = histogram;
    }
    gate_ = gate;
    if (jit) {
        jit_ = true;
        pool_ = reinterpret_cast<uintptr_t>(jit->code_pool().data());
        pool_bytes_ = jit->code_pool().size_bytes();
        gateway_ = reinterpret_cast<uintptr_t>(jit->gateway().data());
        gateway_bytes_ = jit->gateway().size_bytes();
        store_ = reinterpret_cast<uintptr_t>(jit->code_store().data());
        store_bytes_ = jit->code_store().size_bytes();
        gb::sm83_jit_counters = {};
    }
    // The alarm interrupt samples the core that creates the timer.
    gptimer_handle_t timer = nullptr;
    gptimer_config_t config{};
    config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    config.direction = GPTIMER_COUNT_UP;
    config.resolution_hz = 1'000'000;
    if (gptimer_new_timer(&config, &timer) != ESP_OK) return;
    gptimer_event_callbacks_t callbacks{};
    callbacks.on_alarm = on_alarm;
    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = kPeriodUs;
    alarm.flags.auto_reload_on_alarm = true;
    if (gptimer_register_event_callbacks(timer, &callbacks, this) != ESP_OK ||
        gptimer_set_alarm_action(timer, &alarm) != ESP_OK || gptimer_enable(timer) != ESP_OK) {
        gptimer_del_timer(timer);
        return;
    }
    if (gptimer_start(timer) != ESP_OK) {
        gptimer_disable(timer);
        gptimer_del_timer(timer);
        return;
    }
    timer_ = timer;
}

void Profile::start(const gba::Arm7Jit* jit) {
    start(static_cast<const gb::Sm83Jit*>(nullptr));
    if (!text_) return;
    gba_ = true;
    gb::offload_counters = {};
    if (!jit) return;
    pool_ = reinterpret_cast<uintptr_t>(jit->code_pool().data());
    pool_bytes_ = jit->code_pool().size_bytes();
    gateway_ = reinterpret_cast<uintptr_t>(jit->gateway().data());
    gateway_bytes_ = jit->gateway().size_bytes();
    store_ = reinterpret_cast<uintptr_t>(jit->code_store().data());
    store_bytes_ = jit->code_store().size_bytes();
}

void Profile::stop() {
    if (!timer_) return;
    auto timer = static_cast<gptimer_handle_t>(timer_);
    gptimer_stop(timer);
    gptimer_disable(timer);
    gptimer_del_timer(timer);
    timer_ = nullptr;
    if (jit_) counters_ = gb::sm83_jit_counters;
    if (gba_) offload_ = gb::offload_counters;
}

void Profile::record(uint32_t pc) {
    if (gate_ && !gate_->load(std::memory_order_relaxed)) return;
    ++samples_;
    if (pc - text_base() < text_bytes()) ++text_[(pc - text_base()) >> kShift];
    else if (pc - kRamBase < kRamBytes) ++ram_[(pc - kRamBase) >> kShift];
    else if (pc - kRomBase < kRomBytes) ++rom_[(pc - kRomBase) >> kShift];
    else {
        for (size_t i = 0; i < code_count_; ++i) {
            const uintptr_t offset = pc - reinterpret_cast<uintptr_t>(code_[i].words);
            if (offset < code_[i].size_bytes) { ++code_samples_[i][offset >> kCodeShift]; return; }
        }
        ++other_;
    }
}

bool Profile::send(uint8_t backend, unsigned core) const {
    if (!text_ || !ram_) return true;
    auto* payload = static_cast<uint8_t*>(heap_caps_malloc(protocol::kMaxPayload, MALLOC_CAP_SPIRAM));
    if (!payload) return false;
    // LOG frames start with the dropped and truncated log counters, which
    // the runner reads from regular logs only.
    constexpr size_t kStart = 8 + sizeof(kMarker) - 1;
    std::memset(payload, 0, 8);
    std::memcpy(payload + 8, kMarker, sizeof(kMarker) - 1);
    size_t size = kStart;
    bool ok = true;
    const auto flush = [&] {
        if (ok && size > kStart) ok = transport::send(protocol::Kind::log, {payload, size});
        size = kStart;
    };
    char line[256];
    const auto put = [&](int length) {
        if (length <= 0) return;
        length = std::min<int>(length, sizeof(line) - 1);
        if (size + length > protocol::kMaxPayload) flush();
        std::memcpy(payload + size, line, length);
        size += length;
    };
    // The second level's segments each start with a copy of the gateway.
    put(std::snprintf(line, sizeof(line), "job backend=%u core=%u period_us=%lu samples=%lu other=%lu rom=%x:%u "
        "pool=%x:%u gateway=%x:%u store=%x:%u segment=%u\n",
        backend, core, static_cast<unsigned long>(kPeriodUs), static_cast<unsigned long>(samples_),
        static_cast<unsigned long>(other_), static_cast<unsigned>(kRomBase), static_cast<unsigned>(kRomBytes),
        static_cast<unsigned>(pool_), static_cast<unsigned>(pool_bytes_),
        static_cast<unsigned>(gateway_), static_cast<unsigned>(gateway_bytes_), static_cast<unsigned>(store_),
        static_cast<unsigned>(store_bytes_), static_cast<unsigned>(gbx::jit::CodeStore::kSegmentBytes)));
    // The generated code sampled outside internal RAM, as in the console's prof.
    int used = std::snprintf(line, sizeof(line), "code %s", code_count_ ? "" : "-");
    for (size_t i = 0; i < code_count_; ++i)
        used += std::snprintf(line + used, sizeof(line) - used, "%s%x:%x", i ? "," : "",
                              static_cast<unsigned>(reinterpret_cast<uintptr_t>(code_[i].words)),
                              static_cast<unsigned>(code_[i].size_bytes));
    line[used++] = '\n';
    put(used);
    if (jit_) {
        for (const auto& counter : kCounters)
            put(std::snprintf(line, sizeof(line), "counter %s %lu\n", counter.name,
                static_cast<unsigned long>(counters_.*counter.field)));
        for (unsigned i = 0; i < std::size(kExits); ++i)
            put(std::snprintf(line, sizeof(line), "counter exit_%s %lu\n", kExits[i],
                static_cast<unsigned long>(counters_.exits[i])));
    }
    if (gba_) {
        // The first queue holds the PPU's lines.
        put(std::snprintf(line, sizeof(line), "counter render_waits %lu\n",
                          static_cast<unsigned long>(offload_.waits[0])));
        put(std::snprintf(line, sizeof(line), "counter render_wait_us %llu\n",
                          static_cast<unsigned long long>(offload_.wait_us[0])));
    }
    const auto buckets = [&](const uint32_t* histogram, uintptr_t base, size_t bytes, unsigned shift) {
        for (size_t i = 0; i < (bytes >> shift); ++i) if (histogram[i])
            put(std::snprintf(line, sizeof(line), "s %x %lu\n", static_cast<unsigned>(base + (i << shift)),
                static_cast<unsigned long>(histogram[i])));
    };
    buckets(text_, text_base(), text_bytes(), kShift);
    buckets(ram_, kRamBase, kRamBytes, kShift);
    buckets(rom_, kRomBase, kRomBytes, kShift);
    for (size_t i = 0; i < code_count_; ++i)
        buckets(code_samples_[i], reinterpret_cast<uintptr_t>(code_[i].words), code_[i].size_bytes, kCodeShift);
    flush();
    heap_caps_free(payload);
    return ok;
}

}  // namespace gbx::test::profiler
