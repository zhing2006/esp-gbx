#include "rom_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "gbx/gb/system.h"
#include "profiler.h"
#include "transport.h"

namespace gbx::test::rom_test {
namespace {
namespace p = protocol;
using gb::System;
bool error(const char* message);
bool fail(const char* message) { error(message); return false; }
struct Packet {
    // Only the transport owner uses Packet, never either emulation worker.
    // PSRAM leaves internal RAM to the JIT code pool.
    EXT_RAM_BSS_ATTR inline static std::array<uint8_t, p::kMaxPayload> bytes;
    size_t size = 0;
    void u8(uint8_t v) { bytes[size++] = v; }
    void u16(uint16_t v) { p::store16(bytes.data() + size, v); size += 2; }
    void u32(uint32_t v) { p::store32(bytes.data() + size, v); size += 4; }
    void u64(uint64_t v) { u32(v); u32(v >> 32); }
    void data(std::span<const uint8_t> v) { std::memcpy(bytes.data() + size, v.data(), v.size()); size += v.size(); }
    void header(uint8_t type, uint8_t backend, uint32_t id) { u16(kVersion); u8(type); u8(backend); u32(id); }
    void cpu(const gb::Sm83State& s) {
        u16(s.pc); u16(s.sp);
        for (auto v : {s.a,s.f,s.b,s.c,s.d,s.e,s.h,s.l}) u8(v);
        u8(s.ime); u8(s.ei_pending); u8(static_cast<uint8_t>(s.mode)); u8(s.halt_bug);
    }
    bool send() const { return transport::send_data({bytes.data(), size}); }
};
using Digest = std::array<uint8_t, 32>;
bool hash(std::span<const uint8_t> bytes, Digest& digest) {
    size_t written = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes.data(), bytes.size(), digest.data(), digest.size(), &written) == PSA_SUCCESS && written == 32;
}
// One digest of several parts in turn, such as a CGB's banks.
bool hash(std::initializer_list<std::span<const uint8_t>> parts, Digest& digest) {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool valid = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (const auto& part : parts) valid = valid && psa_hash_update(&operation, part.data(), part.size()) == PSA_SUCCESS;
    size_t written = 0;
    valid = valid && psa_hash_finish(&operation, digest.data(), digest.size(), &written) == PSA_SUCCESS && written == 32;
    if (!valid) psa_hash_abort(&operation);
    return valid;
}
// The encoded frame, see FRAME in rom_test.h: the largest is format 2. The
// spans hold the rows the request keeps.
constexpr size_t kFrameBytes = 1 + 2 * gb::kFramePixels;
size_t encode_shades(std::span<const uint8_t> shades, uint8_t* out) {
    out[0] = 0;
    const size_t bytes = shades.size() / 4;
    for (size_t i = 0; i < bytes; ++i)
        out[1 + i] = shades[i * 4] | (shades[i * 4 + 1] << 2) | (shades[i * 4 + 2] << 4) | (shades[i * 4 + 3] << 6);
    return 1 + bytes;
}
// A test frame holds a handful of colors, so a palette keeps it small. It
// runs on the 8 KiB stack of a ROM worker, so the indices are found twice
// instead of kept: the colors are searched from the last one found, which
// most pixels repeat.
size_t encode_colors(std::span<const uint16_t> pixels, uint8_t* out) {
    std::array<uint16_t, 256> colors;
    unsigned count = 0, last = 0;
    const auto find = [&](uint16_t color) {
        if (count && colors[last] == color) return last;
        for (last = 0; last < count && colors[last] != color; ++last) {}
        return last;
    };
    for (const auto color : pixels) {
        if (find(color) < count) continue;
        if (count == colors.size()) {
            out[0] = 2;
            for (size_t k = 0; k < pixels.size(); ++k) p::store16(out + 1 + k * 2, pixels[k]);
            return 1 + 2 * pixels.size();
        }
        colors[count++] = color;
    }
    const unsigned bits = count <= 2 ? 1 : count <= 4 ? 2 : count <= 16 ? 4 : 8;
    out[0] = 1; p::store16(out + 1, static_cast<uint16_t>(count)); out[3] = static_cast<uint8_t>(bits);
    size_t at = 4;
    for (unsigned k = 0; k < count; ++k, at += 2) p::store16(out + at, colors[k]);
    const unsigned per_byte = 8 / bits;
    for (size_t i = 0; i < pixels.size(); i += per_byte) {
        uint8_t byte = 0;
        for (unsigned k = 0; k < per_byte; ++k) byte |= static_cast<uint8_t>(find(pixels[i + k]) << (k * bits));
        out[at++] = byte;
    }
    return at;
}
struct Result {
    uint8_t backend = 0, core = 0, reason = 4;
    Tick time = 0;
    uint64_t wall = 0, instructions = 0;
    gb::Sm83State cpu{};
    uint32_t compiled = 0, invalidated = 0, evicted = 0, code_bytes = 0;
    std::array<uint64_t, 5> fallbacks{};
    std::array<Digest, 6> digests{};
    std::array<uint8_t, 1024> serial{};
    uint16_t serial_size = 0;
    bool serial_truncated = false, capture_frame = false;
    uint8_t frame_rows = 144;  // The top rows of the frame captured.
    uint64_t frame_id = 0;
    Digest frame_digest{};
    std::array<uint8_t, kFrameBytes> frame{};
    size_t frame_size = 0;
    uint64_t pcm_frames = 0, pcm_dropped = 0;
    std::array<uint8_t, 16> pcm_digest{};
    bool has_rtc = false;
    gb::RtcState rtc{};
};
void summarize(System& s, Result& r) {
    r.time = s.memory.clock.now; r.cpu = s.cpu;
    r.serial_size = s.serial().size(); r.serial_truncated = s.serial_truncated();
    std::copy(s.serial().begin(), s.serial().end(), r.serial.begin());
    const auto& m = s.memory;
    const std::span<const uint8_t> vram1 = m.vram1() ? std::span<const uint8_t>(m.vram1(), 8192) : std::span<const uint8_t>{};
    bool valid = hash({m.wram, m.wram_banks()}, r.digests[0]) && hash({m.vram, vram1}, r.digests[1]) &&
        hash(s.memory.oam, r.digests[2]) && hash(std::span(s.memory.high).first(127), r.digests[3]);
    std::array<uint8_t, 129> io{};
    for (unsigned i = 0; i < 128; ++i) io[i] = s.memory.peek(0xff00 + i);
    io[128] = s.memory.peek(0xffff); valid &= hash(io, r.digests[4]);
    valid &= hash(s.memory.cartridge.ram(), r.digests[5]);
    if (r.capture_frame) {
        r.frame_size = 0;
        const size_t kept = r.frame_rows * 160u;
        if (s.mode() == gb::Mode::dmg) {
            const auto frame = s.shades();
            if (frame.size() == gb::kFramePixels) r.frame_size = encode_shades(frame.first(kept), r.frame.data());
        } else {
            const auto frame = s.frame().pixels;
            if (frame.size() == gb::kFramePixels) r.frame_size = encode_colors(frame.first(kept), r.frame.data());
        }
        r.frame_id = s.memory.ppu.frame_id();
        valid &= r.frame_size && hash(std::span(r.frame).first(r.frame_size), r.frame_digest);
    }
    r.has_rtc = s.has_rtc();
    if (r.has_rtc) r.rtc = s.rtc();
    if (!valid) r.reason = 4;
    if (s.jit()) {
        r.compiled = s.jit()->compiled_blocks(); r.invalidated = s.jit()->invalidations();
        r.evicted = s.jit()->evictions(); r.code_bytes = s.jit()->code_bytes(); r.fallbacks = s.jit()->fallbacks();
    }
}
// A RESULT from its reason on, which a STATE RESULT also carries.
void result_body(Packet& out, const Result& r) {
    out.u8(r.reason); out.u8(r.core);
    out.u16(r.serial_truncated | (r.capture_frame ? 2 : 0) | (r.has_rtc ? 4 : 0));
    out.u64(r.time); out.u64(r.wall); out.u64(r.instructions); out.cpu(r.cpu);
    out.u32(r.compiled); out.u32(r.invalidated); out.u32(r.evicted); out.u32(r.code_bytes);
    for (auto value : r.fallbacks) out.u64(value);
    for (const auto& digest : r.digests) out.data(digest);
    out.u64(r.pcm_frames); out.u64(r.pcm_dropped); out.data(r.pcm_digest);
    const auto& c = r.rtc;
    out.u8(c.seconds); out.u8(c.minutes); out.u8(c.hours); out.u16(c.days);
    out.u8(c.halt); out.u8(c.carry); out.data(c.latched); out.u32(c.subsecond);
    if (r.capture_frame) { out.u64(r.frame_id); out.data(r.frame_digest); }
    out.u16(r.serial_size); out.data(std::span(r.serial).first(r.serial_size));
}
bool send_result(const Result& r, uint32_t id) {
    Packet out; out.header(0, r.backend, id); result_body(out, r);
    return out.send();
}
// The encoded frame in parts that fit a payload, see FRAME. A STATE FRAME
// puts the save point and save backend before each part.
bool send_frame(const Result& r, uint32_t id, uint8_t backend, int point = -1, uint8_t save = 0) {
    const size_t room = p::kMaxPayload - 8 - (point >= 0 ? 2 : 0) - 16;
    for (size_t at = 0; at < r.frame_size; at += room) {
        Packet out; out.header(point >= 0 ? 5 : 3, backend, id);
        if (point >= 0) { out.u8(static_cast<uint8_t>(point)); out.u8(save); }
        const size_t part = std::min(room, r.frame_size - at);
        out.u64(r.frame_id); out.u32(static_cast<uint32_t>(r.frame_size)); out.u32(static_cast<uint32_t>(at));
        out.data(std::span(r.frame).subspan(at, part));
        if (!out.send()) return false;
    }
    return true;
}
// The request's setup and script, applied the same way on both backends.
constexpr unsigned kMaxEvents = 128;
struct Script {
    struct Event { Tick at; uint8_t kind, value; };
    gb::Mode mode = gb::Mode::dmg;
    uint8_t frame_rows = 144;
    uint32_t rate = 0;
    bool set_rtc = false, small_pool = false;
    gb::RtcState rtc{};
    std::array<Event, kMaxEvents> events{};
    unsigned count = 0;
};
// A small JIT code pool, when requested, makes blocks move between the
// JIT's two levels all the time.
gb::Sm83Jit::Capacity capacity(const Script& script) {
    return script.small_pool ? gb::Sm83Jit::Capacity{16 * 1024, 64} : gb::Sm83Jit::Capacity{};
}
// The block the JIT's code pool lives in, set aside before the system is
// created, as the product does; it must outlive the system.
platform::ExecutableMemory reserve_jit(const Script& script) {
    return platform::allocate_executable(gb::Sm83Jit::reserved_bytes(capacity(script)));
}
// A system for `script`, whose JIT gets the block of reserve_jit() through
// select_backend().
std::unique_ptr<System> new_system(const Script& script) {
    auto system = std::unique_ptr<System>(new (std::nothrow) System);
    if (system) system->set_jit_capacity(capacity(script));
    return system;
}
void prepare(System& s, const Script& script) {
    s.set_audio_rate(script.rate);
    if (script.set_rtc) s.set_rtc(script.rtc);
}
// Applies the events due by now and returns the time of the next one.
Tick apply(System& s, const Script& script, unsigned& next) {
    for (; next < script.count && script.events[next].at <= s.memory.clock.now; ++next) {
        const auto& event = script.events[next];
        if (event.kind == 0) s.set_buttons(event.value);
        else s.set_rtc(s.rtc());
    }
    return next < script.count ? script.events[next].at : kNever;
}
// Digests the PCM as the owner drains it. The output buffer holds 8192
// frames, which a 65536-tick quantum (about 375 frames at 48 kHz) never
// fills between drains. Draining is not emulation and stays out of the
// wall time.
//
// The digest only tells whether two runs made the same frames, so it need
// not be cryptographic. SHA-256 in the SHA engine ran at about 35 MB/s with
// interrupts masked, near a tenth of the emulating core at the JIT's speed,
// and its cache footprint still slowed the timed part. Four multiply-rotate
// lanes take the 32-bit frames in turn and end in a 128-bit digest.
class PcmDigest {
public:
    void drain(System& s) {
        for (size_t n; (n = s.read_audio(buffer_)); ) {
            unsigned lane = frames_ & 3;
            for (size_t i = 0; i < n; ++i, lane = (lane + 1) & 3) {
                const uint32_t frame = static_cast<uint16_t>(buffer_[i * 2]) |
                    static_cast<uint32_t>(static_cast<uint16_t>(buffer_[i * 2 + 1])) << 16;
                lanes_[lane] = std::rotl(lanes_[lane] + frame * kMultiplier[1], 13) * kMultiplier[0];
            }
            frames_ += n;
        }
    }
    // Takes the rest after the system finished its sound.
    void finish(System& s, Result& r) {
        drain(s);
        for (unsigned i = 0; i < 4; ++i) {
            // Each lane with half of the frame count, each bit spread over the word.
            uint32_t h = lanes_[i] ^ static_cast<uint32_t>(frames_ >> (i & 1) * 32);
            h ^= h >> 15; h *= kMultiplier[1]; h ^= h >> 13; h *= kMultiplier[2]; h ^= h >> 16;
            p::store32(r.pcm_digest.data() + i * 4, h);
        }
        r.pcm_frames = frames_; r.pcm_dropped = s.audio_dropped();
    }
private:
    static constexpr uint32_t kMultiplier[]{0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du};
    std::array<uint32_t, 4> lanes_{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u};
    std::array<int16_t, 512> buffer_{};
    uint64_t frames_ = 0;
};
// The loop of every timed run: applies the script's events, runs quanta of
// at most 65536 ticks and drains the PCM between them, outside the wall time.
struct Run {
    System& system;
    const Script& script;
    PcmDigest& pcm;
    const std::atomic<bool>& cancel;
    std::atomic<uint32_t>& progress;  // Only this worker writes it.
    unsigned next = 0;                // The first script event not applied yet.
    uint64_t instructions = 0;
    uint8_t reason = 4;               // As Result::reason.
    int64_t untimed = 0;              // Draining, in us.
    int64_t yield_at = esp_timer_get_time() + 50'000;
    // Runs to the first instruction boundary at or after `until`. False when
    // the run ended or was cancelled before it.
    bool to(Tick until) {
        while (system.memory.clock.now < until) {
            if (cancel.load(std::memory_order_relaxed)) return false;
            const Tick event = apply(system, script, next);
            const auto result = system.run_until(std::min({until, system.memory.clock.now + 65536, event}));
            instructions += result.instructions;
            progress.store(progress.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (result.end == gb::RunEnd::breakpoint) { reason = 1; return false; }
            if (result.end == gb::RunEnd::suspended) { reason = 3; return false; }
            if (result.end == gb::RunEnd::unavailable) { reason = 4; return false; }
            reason = 2;
            const auto drain = esp_timer_get_time();
            pcm.drain(system);
            const auto now = esp_timer_get_time();
            untimed += now - drain;
            if (now >= yield_at) { vTaskDelay(1); yield_at = esp_timer_get_time() + 50'000; }
        }
        return true;
    }
};
struct Job {
    std::span<const uint8_t> rom;
    Tick limit;
    const Script* script = nullptr;
    uint8_t criterion = 1;
    EventGroupHandle_t events;
    EventBits_t done;
    Result result;
    PcmDigest pcm;
    // Only this counter is sampled while running. Result needs the done bit.
    std::atomic<uint32_t> progress{0};
    std::atomic<bool> cancel{false};
#ifdef GBX_PROFILER
    // The job's core, and core 0 while an isolated job runs on core 1. The
    // worker sets running for the measured run, which other_core counts.
    profiler::Profile profile, other_core;
    std::atomic<bool> running{false};
#endif
};
void worker(void* context) {
    auto& job = *static_cast<Job*>(context);
    auto reserved = job.result.backend ? reserve_jit(*job.script) : platform::ExecutableMemory{};
    auto system = new_system(*job.script);
    const bool ready = system && system->load_rom(job.rom, job.criterion != 3, job.script->mode) &&
        system->select_backend(job.result.backend ? gb::Backend::jit : gb::Backend::interpreter, &reserved);
    if (ready) prepare(*system, *job.script);
    xEventGroupWaitBits(job.events, 1, pdFALSE, pdTRUE, portMAX_DELAY);
    if (ready) {
#ifdef GBX_PROFILER
        job.profile.start(system->jit());
        job.running.store(true, std::memory_order_relaxed);
#endif
        const auto start = esp_timer_get_time();
        Run run{*system, *job.script, job.pcm, job.cancel, job.progress};
        run.to(job.limit);
        job.result.instructions = run.instructions; job.result.reason = run.reason;
        system->memory.ppu.finish();
        system->finish_audio();
        job.result.wall = esp_timer_get_time() - start - run.untimed;
#ifdef GBX_PROFILER
        job.running.store(false, std::memory_order_relaxed);
        job.profile.stop();
#endif
        job.pcm.finish(*system, job.result);
        summarize(*system, job.result);
    }
    else job.result.capture_frame = false;
    system.reset();  // Code pool is retired on its owner core, after execution.
    platform::free_executable(reserved);
    xEventGroupSetBits(job.events, job.done);
    vTaskDelete(nullptr);
}

// Instant saves, see the save point and STATE RESULT in rom_test.h. A save
// point runs once per save backend, in a system of its own on core 1: to the
// target, then one instruction boundary at a time until the conditions
// hold, where the owner captures the state. The transport task on core 0
// encodes it, and decodes it again before each load, as the product's other
// core does, while the owner waits so that the timings stay apart. Each load
// resumes in the save system on one backend, the save backend first, and
// runs on to the end of the uninterrupted runs it is compared with.
constexpr unsigned kMaxSavePoints = 8;
constexpr uint16_t kConditions = 0x1ff;
constexpr Tick kFrameTicks = 140448;
constexpr EventBits_t kStateDone = 1u << 4, kRequest = 1u << 5, kReply = 1u << 6;
struct SavePoint { Tick at; uint32_t window; uint16_t conditions; uint8_t combos; };
enum class StateStatus : uint8_t { resumed, ended, not_met, capture, encoding, decoding, restore, unavailable };
// The conditions in `mask` that hold now. Peeking STAT and NR52 only brings
// the PPU and APU up to now, which an uninterrupted run does as well later.
uint16_t conditions(System& s, uint16_t mask, std::span<const uint8_t> initial_ram) {
    uint16_t met = 0;
    if ((mask & 1) && s.memory.dma_busy()) met |= 1;
    if ((mask & 2) && (s.memory.peek(0xff41) & 3) == 3 && s.memory.ppu.pending_changes()) met |= 2;
    if ((mask & 4) && s.cpu.mode == gb::CpuMode::halted) met |= 4;
    if ((mask & 8) && s.cpu.ei_pending) met |= 8;
    if ((mask & 16) && (s.memory.peek(0xff26) & 15)) met |= 16;
    if ((mask & 32) && s.has_rtc() && s.rtc().subsecond) met |= 32;
    if ((mask & 64) && !initial_ram.empty() && !std::ranges::equal(s.cartridge_ram(), initial_ram)) met |= 64;
    if ((mask & 128) && s.memory.double_speed()) met |= 128;
    if ((mask & 256) && s.memory.hdma_active()) met |= 256;
    return met;
}
gb::Backend backend(uint8_t value) { return value ? gb::Backend::jit : gb::Backend::interpreter; }
struct StateLoad {
    uint8_t backend = 0, decode = 0;
    StateStatus status = StateStatus::unavailable;
    uint32_t decode_us = 0, restore_us = 0;
    std::array<uint32_t, 2> frame_us{}, compiled{}, installed{};
    Result result;
};
struct StateJob {
    std::span<const uint8_t> rom;
    Tick limit = 0;
    const Script* script = nullptr;
    uint8_t criterion = 1, index = 0, save = 0;
    SavePoint point{};
    EventGroupHandle_t events = nullptr;
    // The transport task works on the snapshots and the buffer only between
    // kRequest and kReply, while the worker waits.
    bool decode = false;
    gb::Snapshot saved, loaded;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buffer{nullptr, &heap_caps_free};
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> initial{nullptr, &heap_caps_free};
    size_t buffer_size = 0, encoded = 0;
    uint8_t decode_status = 0;
    uint32_t capture_us = 0, encode_us = 0, decode_us = 0;
    uint16_t met = 0;
    Tick saved_at = 0;
    uint64_t before = 0, dropped = 0;
    unsigned next = 0;       // The first script event the save run had not applied.
    PcmDigest prefix, pcm;   // Up to the save, and of the run going on.
    std::array<StateLoad, 2> loads;
    unsigned load_count = 0;
    platform::ExecutableMemory reserved{};  // For the JIT, see reserve_jit().
    std::atomic<uint32_t> progress{0};
    std::atomic<bool> cancel{false};
};
// Hands the snapshot to the transport task and waits for it.
void ask(StateJob& job, bool decode) {
    job.decode = decode;
    xEventGroupSetBits(job.events, kRequest);
    xEventGroupWaitBits(job.events, kReply, pdTRUE, pdTRUE, portMAX_DELAY);
}
// The transport task's part.
void serve(StateJob& job) {
    const auto start = esp_timer_get_time();
    if (!job.decode) {
        job.encoded = job.saved.encode({job.buffer.get(), job.buffer_size});
        job.encode_us = esp_timer_get_time() - start;
    } else {
        job.decode_status = static_cast<uint8_t>(job.loaded.decode({job.buffer.get(), job.encoded}));
        job.decode_us = esp_timer_get_time() - start;
    }
}
// Runs to the save point and captures and encodes the state there.
StateStatus save(StateJob& job, System& s) {
    const auto& point = job.point;
    const std::span<const uint8_t> initial{job.initial.get(), s.cartridge_ram().size()};
    Run run{s, *job.script, job.pcm, job.cancel, job.progress};
    auto status = run.to(point.at) ? StateStatus::resumed : StateStatus::ended;
    const Tick last = std::min(point.at + point.window, job.limit);
    while (status == StateStatus::resumed && conditions(s, point.conditions, initial) != point.conditions) {
        if (s.memory.clock.now >= last) status = StateStatus::not_met;
        else if (!run.to(s.memory.clock.now + 1)) status = StateStatus::ended;
    }
    job.saved_at = s.memory.clock.now; job.before = run.instructions; job.next = run.next;
    job.met = conditions(s, kConditions, initial);
    for (auto& out : job.loads) out.result.reason = run.reason;
    if (status != StateStatus::resumed) return status;
    const auto start = esp_timer_get_time();
    const bool captured = s.capture(job.saved);
    job.capture_us = esp_timer_get_time() - start;
    if (!captured) return StateStatus::capture;
    // capture() waited for the sound handed to the mixer: this is all of it
    // up to the save.
    job.pcm.drain(s);
    job.prefix = job.pcm; job.dropped = s.audio_dropped();
    ask(job, false);
    return job.encoded ? StateStatus::resumed : StateStatus::encoding;
}
void load(StateJob& job, System& s, StateLoad& out) {
    s.reset();
    // A refused load reports the system it was refused in.
    const auto refuse = [&](StateStatus status) {
        out.status = status; out.result.capture_frame = false;
        summarize(s, out.result);
    };
    // Selecting a backend clears the JIT's code, so the same one is kept.
    if (s.backend() != backend(out.backend) && !s.select_backend(backend(out.backend), &job.reserved))
        return refuse(StateStatus::unavailable);
    ask(job, true);
    out.decode = job.decode_status; out.decode_us = job.decode_us;
    if (out.decode != static_cast<uint8_t>(gb::Snapshot::Status::ok)) return refuse(StateStatus::decoding);
    auto start = esp_timer_get_time();
    const bool restored = s.restore(job.loaded);
    out.restore_us = esp_timer_get_time() - start;
    if (!restored) return refuse(StateStatus::restore);
    out.status = StateStatus::resumed;
    job.pcm = job.prefix;
    Run run{s, *job.script, job.pcm, job.cancel, job.progress};
    run.next = job.next; run.instructions = job.before;
    const auto* jit = s.jit();
    const Tick from = s.memory.clock.now;
    start = esp_timer_get_time();
    bool on = true;
    for (unsigned k = 0; k < 2 && on; ++k) {
        const Tick end = from + (k + 1) * kFrameTicks;
        const uint32_t compiled = jit ? jit->compiled_blocks() : 0, installed = jit ? jit->restored_blocks() : 0;
        const auto begin = esp_timer_get_time();
        const auto untimed = run.untimed;
        on = run.to(std::min(job.limit, end));
        if (on && s.memory.clock.now >= end) {
            out.frame_us[k] = esp_timer_get_time() - begin - (run.untimed - untimed);
            out.compiled[k] = jit ? jit->compiled_blocks() - compiled : 0;
            out.installed[k] = jit ? jit->restored_blocks() - installed : 0;
        }
    }
    if (on) run.to(job.limit);
    s.memory.ppu.finish();
    s.finish_audio();
    out.result.wall = esp_timer_get_time() - start - run.untimed;
    out.result.instructions = run.instructions; out.result.reason = run.reason;
    job.pcm.finish(s, out.result);
    out.result.pcm_dropped += job.dropped;
    summarize(s, out.result);
}
void state_worker(void* context) {
    auto& job = *static_cast<StateJob*>(context);
    // Loads may start the JIT in this system later.
    job.reserved = reserve_jit(*job.script);
    auto system = new_system(*job.script);
    bool ready = system && system->load_rom(job.rom, job.criterion != 3, job.script->mode) &&
        system->select_backend(backend(job.save), &job.reserved);
    if (ready) {
        // Allocated at load, as the product does, so that saving and
        // loading allocate nothing.
        const size_t ram = system->cartridge_ram().size();
        const auto mode = system->memory.mode();
        ready = job.saved.allocate(ram, mode) && job.loaded.allocate(ram, mode);
        job.buffer_size = job.saved.max_encoded();
        if (ready) job.buffer.reset(static_cast<uint8_t*>(heap_caps_malloc(job.buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (ready && ram) job.initial.reset(static_cast<uint8_t*>(heap_caps_malloc(ram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        ready = ready && job.buffer && (!ram || job.initial);
        if (ready) std::copy_n(system->cartridge_ram().data(), ram, job.initial.get());
    }
    auto status = StateStatus::unavailable;
    if (ready) {
        prepare(*system, *job.script);
        status = save(job, *system);
    }
    if (status == StateStatus::resumed) {
        for (unsigned i = 0; i < job.load_count; ++i) load(job, *system, job.loads[i]);
    } else {
        // Every requested load reports where the save run stopped.
        for (unsigned i = 0; i < job.load_count; ++i) {
            auto& out = job.loads[i];
            out.status = status; out.result.capture_frame = false;
            if (ready) summarize(*system, out.result);
            out.result.instructions = job.before;
        }
    }
    system.reset();  // Code pool is retired on its owner core, after execution.
    platform::free_executable(job.reserved);
    xEventGroupSetBits(job.events, kStateDone);
    vTaskDelete(nullptr);
}
bool send_state(const StateJob& job, const StateLoad& load, uint32_t id) {
    Packet out; out.header(4, load.backend, id);
    out.u8(job.index); out.u8(job.save); out.u8(static_cast<uint8_t>(load.status)); out.u8(job.met & 0xff);
    out.u8(load.decode); out.u8(job.met >> 8); out.u16(0);
    out.u64(job.saved_at); out.u64(job.before);
    out.u32(job.encoded); out.u32(job.capture_us); out.u32(job.encode_us); out.u32(load.decode_us); out.u32(load.restore_us);
    for (auto values : {load.frame_us, load.compiled, load.installed}) for (auto v : values) out.u32(v);
    result_body(out, load.result);
    return out.send();
}
// Runs every save point and sends its STATE RESULTs, and a STATE FRAME for
// each resumed frame unlike the uninterrupted one, references[backend].
bool run_states(std::span<const uint8_t> rom, Tick limit, const Script& script, uint8_t criterion,
                std::span<const SavePoint> points, std::array<const Result*, 2> references,
                EventGroupHandle_t events, uint32_t id) {
    struct Release { void operator()(StateJob* job) const { job->~StateJob(); heap_caps_free(job); } };
    for (unsigned index = 0; index < points.size(); ++index) for (uint8_t save = 0; save < 2; ++save) {
        const auto& point = points[index];
        // Bit 0 of the pair loads on INT, bit 1 on JIT.
        const unsigned loads = (point.combos >> save * 2) & 3;
        if (!loads) continue;
        // In PSRAM, which leaves internal RAM to the JIT code pool.
        void* storage = heap_caps_aligned_alloc(alignof(StateJob), sizeof(StateJob), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!storage) return fail("Cannot allocate a save point");
        std::unique_ptr<StateJob, Release> job(new (storage) StateJob);
        job->rom = rom; job->limit = limit; job->script = &script; job->criterion = criterion;
        job->index = index; job->save = save; job->point = point; job->events = events;
        for (const uint8_t target : {save, static_cast<uint8_t>(save ^ 1)}) {
            if (!(loads & (1u << target))) continue;
            auto& out = job->loads[job->load_count++];
            out.backend = target; out.result.backend = target; out.result.core = 1;
            out.result.capture_frame = criterion != 1; out.result.frame_rows = script.frame_rows;
        }
        xEventGroupClearBits(events, kStateDone | kRequest | kReply);
        if (xTaskCreatePinnedToCore(state_worker, "rom-state", 8192, job.get(), 5, nullptr, 1) != pdPASS)
            return fail("Cannot start a save point");
        bool ok = true;
        uint32_t last_progress = 0;
        for (;;) {
            const auto bits = xEventGroupWaitBits(events, kStateDone | kRequest, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
            if (bits & kRequest) {
                xEventGroupClearBits(events, kRequest);
                serve(*job);
                xEventGroupSetBits(events, kReply);
                continue;
            }
            if (bits & kStateDone) break;
            const auto progress = job->progress.load(std::memory_order_relaxed);
            if (!ok || progress == last_progress) continue;
            Packet out; out.header(2, 2, id); out.u64(progress);
            ok = out.send(); last_progress = progress;
            if (!ok) job->cancel.store(true, std::memory_order_relaxed);
        }
        for (unsigned i = 0; ok && i < job->load_count; ++i) {
            const auto& resumed = job->loads[i];
            const auto& r = resumed.result;
            const auto& reference = *references[resumed.backend];
            ok = send_state(*job, resumed, id);
            // The PC has the uninterrupted frame; another one is sent.
            if (ok && r.capture_frame && (!reference.capture_frame || r.frame_id != reference.frame_id ||
                                          r.frame_digest != reference.frame_digest))
                ok = send_frame(r, id, resumed.backend, job->index, job->save);
        }
        if (!ok) return false;
    }
    return true;
}

constexpr size_t kRecords = 128;  // 16 instructions, at most six bus cycles each.
struct Diagnosis {
    std::span<const uint8_t> rom;
    Tick limit;
    const Script* script = nullptr;
    PcmDigest pcm[2];
    SemaphoreHandle_t ready, resume;
    System* systems[2]{};  // Main reads only while the worker is waiting.
    TraceBuffer traces[2];
    std::array<TraceRecord, kRecords> records[2];
    gb::JitResult steps[2];
    Result results[2];
    uint16_t entry = 0;
    uint64_t ordinal = 0;
    bool stop = false, ended = false, initialized = false;
};
void diagnostic_worker(void* context) {
    auto& d = *static_cast<Diagnosis*>(context);
    std::unique_ptr<System> systems[2];
    d.initialized = true;
    // The JIT's block before either system.
    auto reserved = reserve_jit(*d.script);
    for (unsigned i = 0; i < 2; ++i) {
        systems[i] = new_system(*d.script);
        d.systems[i] = systems[i].get();
        d.initialized &= systems[i] && systems[i]->load_rom(d.rom, true, d.script->mode) &&
            systems[i]->select_backend(i ? gb::Backend::jit : gb::Backend::interpreter, &reserved);
        d.results[i].backend = i; d.results[i].core = 1;
    }
    if (d.initialized) {
        for (unsigned i = 0; i < 2; ++i) {
            prepare(*systems[i], *d.script);
            d.traces[i].reset(d.records[i]); systems[i]->set_trace(&d.traces[i]);
        }
        Tick quantum = std::min<Tick>(d.limit, 65536);
        unsigned next[2]{};
        while (!d.stop) {
            // Compared blocks leave both systems at the same time, so both
            // apply an event before the same block.
            Tick event = kNever;
            for (unsigned i = 0; i < 2; ++i) event = apply(*systems[i], *d.script, next[i]);
            const Tick until = std::min(quantum, event);
            for (unsigned i = 0; i < 2; ++i) d.traces[i].reset(d.records[i]);
            d.entry = systems[1]->cpu.pc;
            auto start = esp_timer_get_time();
            d.steps[1] = systems[1]->step_block(until);
            d.results[1].wall += esp_timer_get_time() - start;
            start = esp_timer_get_time();
            d.steps[0] = systems[0]->step_block(until, d.steps[1].instructions);
            d.results[0].wall += esp_timer_get_time() - start;
            if (systems[1]->memory.clock.now >= quantum) {
                quantum = std::min(d.limit, systems[1]->memory.clock.now + 65536);
                for (unsigned i = 0; i < 2; ++i) d.pcm[i].drain(*systems[i]);
            }
            ++d.ordinal;
            for (unsigned i = 0; i < 2; ++i) d.results[i].instructions += d.steps[i].instructions;
            d.ended = d.steps[1].exit == gb::JitExit::breakpoint || systems[1]->memory.clock.now >= d.limit ||
                (d.steps[1].exit == gb::JitExit::suspended && !d.steps[1].instructions) ||
                d.steps[1].fallback != gb::JitFallback::none;
            xSemaphoreGive(d.ready);
            xSemaphoreTake(d.resume, portMAX_DELAY);
        }
        // The results were taken at the last block; the sound up to then
        // completes them.
        for (unsigned i = 0; i < 2; ++i) {
            systems[i]->finish_audio();
            d.pcm[i].finish(*systems[i], d.results[i]);
        }
    }
    systems[0].reset(); systems[1].reset();
    platform::free_executable(reserved);
    d.systems[0] = d.systems[1] = nullptr;
    xSemaphoreGive(d.ready);
    vTaskDelete(nullptr);
}
bool same_cpu(const gb::Sm83State& a, const gb::Sm83State& b) {
    return a.af() == b.af() && a.bc() == b.bc() && a.de() == b.de() && a.hl() == b.hl() &&
        a.sp == b.sp && a.pc == b.pc && a.ime == b.ime && a.ei_pending == b.ei_pending &&
        a.mode == b.mode && a.halt_bug == b.halt_bug;
}
bool diagnose(std::span<const uint8_t> rom, Tick limit, const Script& script, uint32_t id) {
    auto d = std::unique_ptr<Diagnosis>(new (std::nothrow) Diagnosis);
    if (!d) return fail("Cannot allocate ROM diagnosis");
    d->rom = rom; d->limit = limit; d->script = &script;
    d->ready = xSemaphoreCreateBinary(); d->resume = xSemaphoreCreateBinary();
    if (!d->ready || !d->resume) {
        if (d->ready) vSemaphoreDelete(d->ready);
        if (d->resume) vSemaphoreDelete(d->resume);
        return fail("Cannot allocate diagnostic synchronization");
    }
    if (xTaskCreatePinnedToCore(diagnostic_worker, "rom-lockstep", 8192, d.get(), 5, nullptr, 1) != pdPASS) {
        vSemaphoreDelete(d->ready); vSemaphoreDelete(d->resume);
        return fail("Cannot start diagnostic worker");
    }
    bool ok = true;
    auto progress_at = esp_timer_get_time() + 1'000'000;
    for (;;) {
        xSemaphoreTake(d->ready, portMAX_DELAY);
        if (!d->systems[0]) break;
        auto& a = *d->systems[0]; auto& b = *d->systems[1];
        std::array<TraceRecord, kRecords> records[2];
        size_t counts[2]{};
        for (unsigned i = 0; i < 2; ++i) while (counts[i] < kRecords && d->traces[i].pop(records[i][counts[i]])) ++counts[i];
        uint8_t kind = 0, av = 0, bv = 0;
        uint16_t address = 0;
        uint32_t first = UINT32_MAX;
        if (d->traces[0].dropped() || d->traces[1].dropped()) kind = 5;
        for (size_t i = 0; !kind && i < std::max(counts[0], counts[1]); ++i) {
            const auto& x = records[0][i]; const auto& y = records[1][i];
            if (i >= counts[0] || i >= counts[1] || x.time != y.time || x.pc != y.pc ||
                x.address != y.address || x.data != y.data || x.operation != y.operation) {
                kind = 3; first = i;
            }
        }
        for (unsigned side = 0; !kind && side < 2; ++side) for (size_t i = 0; !kind && i < counts[side]; ++i) {
            if (records[side][i].operation != 1) continue;
            address = records[side][i].address; av = a.memory.peek(address); bv = b.memory.peek(address);
            if (av != bv) kind = 4;
        }
        if (!kind && a.memory.clock.now != b.memory.clock.now) kind = 2;
        if (!kind && (!same_cpu(a.cpu, b.cpu) || d->steps[0].instructions != d->steps[1].instructions)) kind = 1;
        if (kind) {
            Packet out; out.header(1, 2, id); out.u64(d->ordinal); out.u16(d->entry);
            out.u16(b.memory.instruction_pc); out.u32(d->steps[1].instructions);
            out.u8(kind); out.u32(first); out.u16(address); out.u8(av); out.u8(bv);
            out.cpu(a.cpu); out.cpu(b.cpu); out.u64(a.memory.clock.now); out.u64(b.memory.clock.now);
            out.u16(counts[0]); out.u16(counts[1]);
            for (unsigned side = 0; side < 2; ++side) for (size_t i = 0; i < counts[side]; ++i) {
                const auto& r = records[side][i];
                out.u64(r.time); out.u16(r.pc); out.u16(r.address); out.u8(r.data); out.u8(r.operation); out.u16(0);
            }
            ok = out.send();
        }
        if (!kind && !d->ended && esp_timer_get_time() >= progress_at) {
            Packet out; out.header(2, 2, id); out.u64(d->ordinal);
            ok = out.send(); progress_at = esp_timer_get_time() + 1'000'000;
        }
        d->stop = kind || d->ended || !ok;
        if (d->stop) for (unsigned i = 0; i < 2; ++i) {
            d->results[i].reason = d->steps[i].exit == gb::JitExit::breakpoint ? 1 :
                d->systems[i]->cpu.mode != gb::CpuMode::running ? 3 : 2;
            summarize(*d->systems[i], d->results[i]);
        }
        xSemaphoreGive(d->resume);
    }
    vSemaphoreDelete(d->ready); vSemaphoreDelete(d->resume);
    if (!d->initialized) return fail("Cannot initialize diagnostic systems");
    return ok && send_result(d->results[0], id) && send_result(d->results[1], id);
}
bool error(const char* message) {
    return transport::send(p::Kind::error, {reinterpret_cast<const uint8_t*>(message), std::strlen(message)});
}
}  // namespace

bool run(std::span<const uint8_t> request) {
    if (request.size() < 76 || p::load16(request.data()) != kVersion || request[2] > 4 || request[3] < 1 || request[3] > 3 ||
        p::load16(request.data() + 22) > kMaxEvents || p::load16(request.data() + 24) > kMaxSavePoints ||
        request[26] > 2 || request[27] > 144) return error("Invalid ROM request or protocol version");
    if (request[2] == 3 && request[3] != 1) return error("Lockstep requires the register criterion");
    const auto criterion = request[3];
    const auto mode = request[2]; const auto id = p::load32(request.data() + 4);
    const Tick limit = p::load32(request.data() + 8) | (Tick{p::load32(request.data() + 12)} << 32);
    const auto size = p::load32(request.data() + 16); const auto path_size = p::load16(request.data() + 20);
    const unsigned events = p::load16(request.data() + 22), saves = p::load16(request.data() + 24);
    const size_t points_at = 76u + events * 12u, path_at = points_at + saves * 16u;
    if (!id || !limit || limit > 120 * kTicksPerSecond || size < 32768 || size > 8388608 || path_size > 240 ||
        request.size() != path_at + path_size) return error("Invalid ROM size, path or deadline");
    // The save points compare with the paired runs on both cores in turn.
    if (saves && mode != 4) return error("Save points require paired sequential runs");
    auto script = std::unique_ptr<Script>(new (std::nothrow) Script);
    if (!script) return error("Cannot allocate ROM script");
    script->mode = request[26] == 1 ? gb::Mode::cgb : request[26] == 2 ? gb::Mode::compat : gb::Mode::dmg;
    script->frame_rows = request[27] ? request[27] : 144;
    const uint8_t* setup = request.data() + 60;
    auto& clock = script->rtc;
    script->rate = p::load32(setup); script->set_rtc = setup[4] & 1; script->small_pool = setup[4] & 2;
    clock.seconds = setup[5]; clock.minutes = setup[6]; clock.hours = setup[7];
    clock.days = p::load16(setup + 8); clock.halt = setup[10] & 1; clock.carry = setup[11] & 1;
    clock.subsecond = p::load32(setup + 12);
    bool valid = script->rate <= 192000 && setup[4] <= 3 && clock.seconds < 64 && clock.minutes < 64 &&
        clock.hours < 32 && clock.days < 512 && setup[10] <= 1 && setup[11] <= 1 && clock.subsecond < kTicksPerSecond &&
        (script->set_rtc || std::all_of(setup + 5, setup + 16, [](uint8_t v) { return v == 0; }));
    script->count = events;
    for (unsigned i = 0; valid && i < events; ++i) {
        const uint8_t* e = request.data() + 76 + i * 12;
        auto& event = script->events[i];
        event = {p::load32(e) | (Tick{p::load32(e + 4)} << 32), e[8], e[9]};
        valid = event.at < limit && (!i || event.at >= script->events[i - 1].at) && event.kind <= 1 &&
            (event.kind == 0 || event.value == 0) && p::load16(e + 10) == 0;
    }
    std::array<SavePoint, kMaxSavePoints> points{};
    unsigned loads = 0;
    for (unsigned i = 0; valid && i < saves; ++i) {
        const uint8_t* s = request.data() + points_at + i * 16;
        auto& point = points[i];
        point = {p::load32(s) | (Tick{p::load32(s + 4)} << 32), p::load32(s + 8),
                 static_cast<uint16_t>(s[12] | s[14] << 8), s[13]};
        valid = point.at < limit && point.window <= kTicksPerSecond && point.conditions <= kConditions &&
            point.combos && point.combos <= 15 && s[15] == 0;
        loads += std::popcount(point.combos);
    }
    if (!valid) return error("Invalid ROM setup, script or save point");
    char relative[241]{}; std::memcpy(relative, request.data() + path_at, path_size);
    if (std::strlen(relative) != path_size || std::strncmp(relative, "roms/", 5) ||
        std::strstr(relative, "..") || std::strchr(relative, '\\')) return error("Invalid ROM path");
    char path[272]; std::snprintf(path, sizeof(path), "/sdcard/tests/gb/%s", relative);
    auto rom = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>(
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
    if (!rom) return error("Cannot allocate ROM in PSRAM");
    auto* file = std::fopen(path, "rb");
    if (!file) return error("Cannot open ROM on SD card");
    valid = std::fread(rom.get(), 1, size, file) == size && std::fgetc(file) == EOF && !std::ferror(file);
    std::fclose(file);
    Digest digest;
    valid = valid && hash({rom.get(), size}, digest) && std::memcmp(digest.data(), request.data() + 28, 32) == 0;
    if (!valid) return error("ROM content does not match the requested manifest entry");
    bool ok = true;
    unsigned count = mode == 1 || mode == 2 ? 1 : 2;
    if (mode == 3) ok = diagnose({rom.get(), size}, limit, *script, id);
    else {
        auto events = xEventGroupCreate();
        if (!events) return error("Cannot allocate ROM worker events");
        auto jobs = std::unique_ptr<Job[]>(new (std::nothrow) Job[2]);
        if (!jobs) { vEventGroupDelete(events); return error("Cannot allocate ROM jobs"); }
        unsigned created = 0;
        // Mode 4 gives each backend both cores in turn and retains both results
        // so identical frames need only one transfer. Mode 0 tests contention.
        const unsigned stages = mode == 4 ? count : 1;
        for (unsigned stage = 0; stage < stages && ok; ++stage) {
            EventBits_t mask = 0;
            const unsigned first = mode == 4 ? stage : 0;
            const unsigned last = mode == 4 ? stage + 1 : count;
            for (unsigned i = first; i < last; ++i) {
                const unsigned backend = count == 2 ? i : mode - 1;
                auto& job = jobs[i]; job.rom = {rom.get(), size}; job.limit = limit; job.script = script.get();
                job.criterion = criterion; job.result.capture_frame = criterion != 1;
                job.result.frame_rows = script->frame_rows;
                job.events = events; job.done = 2u << i; job.result.backend = backend;
                job.result.core = mode == 0 ? i : 1;
                if (xTaskCreatePinnedToCore(worker, backend ? "rom-jit" : "rom-int", 8192, &job, 5, nullptr, job.result.core) != pdPASS) break;
                ++created; mask |= job.done;
            }
            if (created != last) for (unsigned i = first; i < created; ++i)
                jobs[i].cancel.store(true, std::memory_order_relaxed);
#ifdef GBX_PROFILER
            if (mode == 4) jobs[first].other_core.start(nullptr, &jobs[first].running);
#endif
            xEventGroupSetBits(events, 1);
            uint64_t last_progress = 0;
            while (mask && (xEventGroupWaitBits(events, mask, pdFALSE, pdTRUE, pdMS_TO_TICKS(500)) & mask) != mask) {
                if (!ok) continue;
                uint64_t progress = 0;
                for (unsigned i = 0; i < created; ++i)
                    progress += jobs[i].progress.load(std::memory_order_relaxed);
                if (progress == last_progress) continue;
                Packet out; out.header(2, 2, id); out.u64(progress);
                ok = out.send(); last_progress = progress;
                if (!ok) for (unsigned i = first; i < created; ++i)
                    jobs[i].cancel.store(true, std::memory_order_relaxed);
            }
#ifdef GBX_PROFILER
            if (mode == 4) jobs[first].other_core.stop();
#endif
            ok = ok && created == last;
        }
        // Join on errors too: workers own the ROM and job storage until done.
        ok = ok && created == count;
        for (unsigned i = 0; ok && i < count; ++i) ok = send_result(jobs[i].result, id);
#ifdef GBX_PROFILER
        for (unsigned i = 0; ok && i < count; ++i) {
            ok = jobs[i].profile.send(jobs[i].result.backend, jobs[i].result.core);
            if (ok) ok = jobs[i].other_core.send(jobs[i].result.backend, 0);
        }
#endif
        if (ok && criterion != 1) {
            const bool shared = count == 2 && jobs[0].result.capture_frame && jobs[1].result.capture_frame && jobs[0].result.frame_digest == jobs[1].result.frame_digest &&
                jobs[0].result.frame_id == jobs[1].result.frame_id;
            if (shared) ok = send_frame(jobs[0].result, id, 2);
            else for (unsigned i = 0; ok && i < count; ++i)
                if (jobs[i].result.capture_frame) ok = send_frame(jobs[i].result, id, jobs[i].result.backend);
        }
        if (ok && saves) ok = run_states({rom.get(), size}, limit, *script, criterion, std::span(points).first(saves),
                                         {&jobs[0].result, &jobs[1].result}, events, id);
        vEventGroupDelete(events);
        if (created != count) return fail("Cannot start all requested ROM workers");
    }
    if (!ok) return false;
    if (!transport::finish()) return false;
    uint8_t done[8]; p::store16(done, kVersion); p::store16(done + 2, count + loads); p::store32(done + 4, id);
    return transport::send(p::Kind::done, done);
}
}  // namespace gbx::test::rom_test
