#include "gba_rom_test.h"

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
#include "freertos/task.h"
#include "psa/crypto.h"
#include "gbx/gba/jit/arm7_jit.h"
#include "gbx/gba/system.h"
#include "gbx/platform.h"
#include "profiler.h"
#include "transport.h"

namespace gbx::test::gba_rom_test {
namespace {
namespace p = protocol;
using gba::System;
using Digest = std::array<uint8_t, 32>;

bool error(const char* message) {
    return transport::send(p::Kind::error, {reinterpret_cast<const uint8_t*>(message), std::strlen(message)});
}
bool fail(const char* message) {
    error(message);
    return false;
}

// The BIOS, read once at start-up. Internal RAM: the interpreter reads it
// for every BIOS opcode and access. Static, so that it stays out of the
// heap in high SRAM: the JIT's code pool needs nearly all that is free
// there once a ROM larger than the cache keeps its file open.
alignas(4) uint8_t bios[gba::kBiosBytes];
bool bios_valid = false;

struct Packet {
    // Only the transport owner uses Packet, never the emulation worker.
    // PSRAM leaves internal RAM to the JIT code pool.
    EXT_RAM_BSS_ATTR inline static std::array<uint8_t, p::kMaxPayload> bytes;
    size_t size = 0;
    void u8(uint8_t v) { bytes[size++] = v; }
    void u16(uint16_t v) { p::store16(bytes.data() + size, v); size += 2; }
    void u32(uint32_t v) { p::store32(bytes.data() + size, v); size += 4; }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    void data(std::span<const uint8_t> v) { std::memcpy(bytes.data() + size, v.data(), v.size()); size += v.size(); }
    void header(uint8_t type, uint8_t backend, uint32_t id) { u16(kVersion); u8(type); u8(backend); u32(id); }
    bool send() const { return transport::send_data({bytes.data(), size}); }
};

bool hash(std::initializer_list<std::span<const uint8_t>> parts, Digest& digest) {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool valid = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (const auto& part : parts) valid = valid && psa_hash_update(&operation, part.data(), part.size()) == PSA_SUCCESS;
    size_t written = 0;
    valid = valid && psa_hash_finish(&operation, digest.data(), digest.size(), &written) == PSA_SUCCESS && written == 32;
    if (!valid) psa_hash_abort(&operation);
    return valid;
}

// The final frame, see FRAME: a palette of the colors in use, or the
// pixels themselves past 256 colors.
constexpr size_t kFramePixels = 240 * 160, kFrameBytes = 1 + 2 * kFramePixels;
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
    out[0] = 1;
    p::store16(out + 1, static_cast<uint16_t>(count));
    out[3] = static_cast<uint8_t>(bits);
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

constexpr unsigned kTextRows = 20, kTextColumns = 30;
constexpr size_t kTextBytes = kTextRows * kTextColumns, kBitmapBytes = 8 * 240;

struct Cpu {
    gba::Arm7Registers registers{};
    std::array<uint32_t, 2> pipeline{};
    bool sequential = false, halted = false;
};
struct Result {
    uint8_t backend = 0, core = 1, reason = 4;
    Tick time = 0;
    uint64_t wall = 0;
    Cpu cpu{};
    std::array<uint32_t, 7> jit{};
    std::array<uint64_t, gba::kArm7JitFallbacks> fallbacks{};
    std::array<Digest, 7> digests{};
    uint64_t misses = 0, miss_us = 0, ahead = 0, halt_cycles = 0;
    uint64_t pcm_frames = 0, pcm_dropped = 0;
    std::array<uint8_t, 16> pcm_digest{};
    bool has_rtc = false;
    gba::RtcState rtc{};
    std::array<uint8_t, kTextBytes> text{};
    std::array<uint8_t, kBitmapBytes> bitmap{};
    bool capture_frame = false;
    uint64_t frame_id = 0;
    Digest frame_digest{};
    size_t frame_size = 0;
    uint8_t* frame = nullptr;  // kFrameBytes in PSRAM, owned by the job.
#ifdef GBX_PROFILER
    // The run's core, with the JIT's code regions, and core 0, where the
    // renderer works, while `running`.
    profiler::Profile profile, other_core;
    std::atomic<bool> running{false};
#endif
};

Cpu cpu_of(const System& s) {
    return {gba::arm7_registers(s.cpu), s.cpu.pipeline, s.cpu.sequential, s.memory.halted};
}

void console_text(System& s, std::span<uint8_t, kTextBytes> out) {
    const auto vram = s.memory.vram_bytes();
    for (unsigned row = 0; row < kTextRows; ++row)
        for (unsigned column = 0; column < kTextColumns; ++column)
            out[row * kTextColumns + column] = vram[0x2000 + (row * 32 + column) * 2];
}

void summarize(System& s, Result& r) {
    auto& m = s.memory;
    r.time = s.now();
    r.cpu = cpu_of(s);
    std::array<uint8_t, 0x400> io{};
    m.io_snapshot(io);
    bool valid = hash({m.ewram_bytes()}, r.digests[0]) && hash({m.iwram}, r.digests[1]) &&
                 hash({m.vram_bytes()}, r.digests[2]) && hash({m.palette}, r.digests[3]) &&
                 hash({m.oam}, r.digests[4]) && hash({io}, r.digests[5]) && hash({m.backup.data()}, r.digests[6]);
    console_text(s, r.text);
    std::memcpy(r.bitmap.data(), m.vram_bytes().data() + 76 * 240, kBitmapBytes);
    r.halt_cycles = m.halt_cycles;
    const auto& stats = m.rom->stats();
    r.misses = stats.misses;
    r.miss_us = stats.miss_us;
    r.ahead = stats.ahead;
    if (r.capture_frame) {
        const auto pixels = m.ppu.pixels();
        r.frame_size = pixels.size() == kFramePixels ? encode_colors(pixels, r.frame) : 0;
        r.frame_id = m.ppu.frame_id();
        valid = valid && r.frame_size && hash({std::span<const uint8_t>(r.frame, r.frame_size)}, r.frame_digest);
    }
    if (const auto* jit = s.jit()) {
        r.jit = {jit->compiled_blocks(), jit->invalidations(), jit->evictions(), jit->restored_blocks(),
                 jit->stored_entries(), jit->store_drops(), static_cast<uint32_t>(jit->code_bytes())};
        r.fallbacks = jit->fallbacks();
    }
    r.has_rtc = s.has_rtc();
    if (r.has_rtc) r.rtc = s.rtc_state();
    if (!valid) r.reason = 4;
}

void put_cpu(Packet& out, const Cpu& c) {
    const auto& g = c.registers;
    for (auto v : g.r) out.u32(v);
    for (auto v : g.fiq) out.u32(v);
    for (const auto* bank : {&g.svc, &g.abt, &g.irq, &g.und})
        for (auto v : *bank) out.u32(v);
    out.u32(g.cpsr);
    for (auto v : g.spsr) out.u32(v);
    out.u32(c.pipeline[0]);
    out.u32(c.pipeline[1]);
    out.u8(c.sequential);
    out.u8(c.halted);
    out.u16(0);
}

// A RESULT from its reason on, which a STATE RESULT also carries.
void result_body(Packet& out, const Result& r) {
    const bool frame = r.capture_frame && r.frame_size;
    out.u8(r.reason);
    out.u8(r.core);
    out.u16((frame ? 1 : 0) | (r.has_rtc ? 2 : 0));
    out.u64(r.time);
    out.u64(r.wall);
    put_cpu(out, r.cpu);
    for (auto v : r.jit) out.u32(v);
    for (auto v : r.fallbacks) out.u64(v);
    for (const auto& d : r.digests) out.data(d);
    out.u64(r.misses);
    out.u64(r.miss_us);
    out.u64(r.ahead);
    out.u64(r.halt_cycles);
    out.u64(r.pcm_frames);
    out.u64(r.pcm_dropped);
    out.data(r.pcm_digest);
    const gba::RtcState rtc = r.has_rtc ? r.rtc : gba::RtcState{{}, 0, 0};
    out.data(rtc.datetime);
    out.u8(rtc.control);
    out.u32(rtc.subsecond);
    out.data(r.text);
    out.data(r.bitmap);
    if (frame) {
        out.u64(r.frame_id);
        out.data(r.frame_digest);
    }
}

bool send_result(const Result& r, uint32_t id) {
    Packet out;
    out.header(0, r.backend, id);
    result_body(out, r);
    return out.send();
}

// The encoded frame in parts that fit a payload, see FRAME. A STATE FRAME
// puts the save point and save backend before each part.
bool send_frame(const Result& r, uint32_t id, uint8_t backend, int point = -1, uint8_t save = 0) {
    const size_t room = p::kMaxPayload - 8 - (point >= 0 ? 2 : 0) - 16;
    for (size_t at = 0; at < r.frame_size; at += room) {
        Packet out;
        out.header(point >= 0 ? 5 : 3, backend, id);
        if (point >= 0) {
            out.u8(static_cast<uint8_t>(point));
            out.u8(save);
        }
        const size_t part = std::min(room, r.frame_size - at);
        out.u64(r.frame_id);
        out.u32(static_cast<uint32_t>(r.frame_size));
        out.u32(static_cast<uint32_t>(at));
        out.data({r.frame + at, part});
        if (!out.send()) return false;
    }
    return true;
}

// Digests the PCM as the owner drains it. The mixer's output holds 8192
// frames, which a quantum of at most 65536 ticks (about 190 frames at
// 48 kHz) never fills between drains. Draining is not emulation and stays
// out of the wall time.
//
// The digest only tells whether two runs made the same frames, so it need
// not be cryptographic: as in tests/device/rom_test.cpp, four
// multiply-rotate lanes take the 32-bit frames in turn and end in a 128-bit
// digest.
class PcmDigest {
public:
    void drain(System& s) {
        for (size_t n; (n = s.read_audio(buffer_));) {
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
            h ^= h >> 15;
            h *= kMultiplier[1];
            h ^= h >> 13;
            h *= kMultiplier[2];
            h ^= h >> 16;
            p::store32(r.pcm_digest.data() + i * 4, h);
        }
        r.pcm_frames = frames_;
        r.pcm_dropped = s.audio_dropped();
    }

private:
    static constexpr uint32_t kMultiplier[]{0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du};
    std::array<uint32_t, 4> lanes_{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u};
    std::array<int16_t, 512> buffer_{};
    uint64_t frames_ = 0;
};

// Lockstep (mode 3): the JIT's system runs the script, and at each turn
// of Arm7Jit::run()'s loop its observer runs the interpreter's system to
// the same count of instructions and stops, in the loop of
// arm7_run_mapped(), and compares the CPUs and the times. Memory is
// compared at the end of each quantum. A difference there replays the run
// from power-on with the regions that differed compared at each turn of
// that quantum. I/O is compared at quantum ends only: its snapshot moves
// the devices' clock.
struct Diff {
    uint64_t turn = 0, quantum = 0;
    // The JIT's state at the turn before: R15, CPSR, instructions, time.
    uint32_t r15 = 0, cpsr = 0, before = 0;
    Tick since = 0;
    std::array<uint32_t, 2> instructions{}, stops{};  // Interpreter, JIT.
    uint8_t kind = 0, region = 0;  // kind: 1 CPU, 2 time, 3 memory, 4 turn.
    uint32_t offset = 0;
    std::array<uint8_t, 2> values{};
    std::array<Tick, 2> ticks{};
    std::array<Cpu, 2> cpu{};
};

struct Lockstep {
    Lockstep(System& jit_system, System& interpreter_system) : jit(jit_system), interpreter(interpreter_system) {}
    System& jit;
    System& interpreter;
    PcmDigest* pcm = nullptr;  // The interpreter's.
    // Memory regions (bit = region number) compared at each turn of
    // quantum `detail`.
    uint64_t detail = UINT64_MAX;
    uint8_t regions = 0;
    uint64_t quantum = 0, turn = 0;
    uint32_t instructions = 0, stops = 0;
    uint32_t r15 = 0, cpsr = 0, before = 0;
    Tick since = 0;
    bool diverged = false;
    uint8_t differing = 0;  // Regions that differed at a quantum's end.
    Diff diff;

    static std::span<const uint8_t> region(const System& s, unsigned number) {
        switch (number) {
        case 2: return s.memory.ewram_bytes();
        case 3: return s.memory.iwram;
        case 5: return s.memory.palette;
        case 6: return s.memory.vram_bytes();
        case 7: return s.memory.oam;
        default: return {};
        }
    }
    void begin(Tick target) {
        interpreter.memory.begin_run(target);
        instructions = stops = 0;
    }
    // The interpreter to the JIT's turn; false when it cannot reach it.
    bool mirror(uint32_t want_instructions, uint32_t want_stops, bool end) {
        auto& m = interpreter.memory;
        auto& cpu = interpreter.cpu;
        for (;;) {
            if (m.left <= 0 && !m.stop) m.catch_up_boundary();
            if (!end && instructions == want_instructions && stops == want_stops) return true;
            if (stops > want_stops) return false;
            // A stop the JIT alone takes, after a write to translated code,
            // is taken here too: with nothing due it changes nothing.
            if (m.stop || (instructions == want_instructions && stops < want_stops)) {
                m.stop = false;
                ++stops;
                if (m.halted) {
                    m.run_halt();
                    if (m.halted) return end && instructions == want_instructions && stops == want_stops;
                }
                if (m.irq_line && (!(cpu.cpsr & gba::kFlagI) || m.irq_unmasked)) gba::arm7_interrupt_mapped(cpu, m);
                m.irq_unmasked = false;
                if (m.now() >= m.target) {
                    m.stop = true;
                    return end && instructions == want_instructions && stops == want_stops;
                }
                m.catch_up_boundary();
                continue;
            }
            if (instructions == want_instructions) return false;
            gba::arm7_step_mapped(cpu, m);
            ++instructions;
        }
    }
    void record(uint8_t kind, uint32_t jit_instructions, uint32_t jit_stops) {
        diverged = true;
        diff.turn = turn;
        diff.quantum = quantum;
        diff.r15 = r15;
        diff.cpsr = cpsr;
        diff.before = before;
        diff.since = since;
        diff.instructions = {instructions, jit_instructions};
        diff.stops = {stops, jit_stops};
        diff.kind = kind;
        diff.ticks = {interpreter.now(), jit.now()};
        diff.cpu = {cpu_of(interpreter), cpu_of(jit)};
    }
    // The first differing byte of the regions in `mask`.
    bool memory_differs(uint8_t mask) {
        for (unsigned number = 2; number < 8; ++number) {
            if (!(mask >> number & 1)) continue;
            const auto a = region(interpreter, number), b = region(jit, number);
            if (a.empty() || !std::memcmp(a.data(), b.data(), a.size())) continue;
            size_t at = 0;
            while (a[at] == b[at]) ++at;
            diff.region = static_cast<uint8_t>(number);
            diff.offset = static_cast<uint32_t>(at);
            diff.values = {a[at], b[at]};
            return true;
        }
        return false;
    }
    static void observe(void* self, uint32_t jit_instructions, uint32_t jit_stops, bool end) {
        auto& l = *static_cast<Lockstep*>(self);
        if (l.diverged) return;
        const bool reached = l.mirror(jit_instructions, jit_stops, end);
        ++l.turn;
        const Cpu a = cpu_of(l.interpreter), b = cpu_of(l.jit);
        const bool cpu_differs = std::memcmp(&a.registers, &b.registers, sizeof(a.registers)) ||
                                 a.pipeline != b.pipeline || a.sequential != b.sequential || a.halted != b.halted;
        uint8_t kind = !reached ? 4 : cpu_differs ? 1 : l.interpreter.now() != l.jit.now() ? 2 : 0;
        if (!kind && l.quantum == l.detail && l.memory_differs(l.regions)) kind = 3;
        if (kind) l.record(kind, jit_instructions, jit_stops);
        l.r15 = l.jit.cpu.r[15];
        l.cpsr = l.jit.cpu.cpsr;
        l.before = jit_instructions;
        l.since = l.jit.now();
    }
    // After each quantum: false once the systems differ.
    bool end_quantum() {
        if (diverged) return false;
        differing = 0;
        for (unsigned number = 2; number < 8; ++number) {
            const auto a = region(interpreter, number), b = region(jit, number);
            if (!a.empty() && std::memcmp(a.data(), b.data(), a.size())) differing |= 1 << number;
        }
        std::array<uint8_t, 0x400> io[2];
        interpreter.memory.io_snapshot(io[0]);
        jit.memory.io_snapshot(io[1]);
        if (io[0] != io[1]) differing |= 1 << 4;
        if (differing) {
            record(3, instructions, stops);
            if (!memory_differs(differing)) {
                size_t at = 0;
                while (io[0][at] == io[1][at]) ++at;
                diff.region = 4;
                diff.offset = static_cast<uint32_t>(at);
                diff.values = {io[0][at], io[1][at]};
            }
            return false;
        }
        ++quantum;
        return true;
    }
};

bool send_diff(const Diff& d, uint32_t id) {
    Packet out;
    out.header(1, 2, id);
    out.u64(d.turn);
    out.u64(d.quantum);
    out.u32(d.r15);
    out.u32(d.cpsr);
    out.u32(d.before);
    out.u64(d.since);
    for (auto v : d.instructions) out.u32(v);
    for (auto v : d.stops) out.u32(v);
    out.u8(d.kind);
    out.u8(d.region);
    out.u16(0);
    out.u32(d.offset);
    out.u8(d.values[0]);
    out.u8(d.values[1]);
    out.u16(0);
    for (auto v : d.ticks) out.u64(v);
    for (const auto& c : d.cpu) put_cpu(out, c);
    return out.send();
}

constexpr unsigned kMaxEvents = 64;
struct Script {
    struct Event { Tick at; uint16_t keys; };
    uint8_t end = 1;
    uint32_t frames = 0;
    std::array<char, 65> markers{};  // LF-separated, NUL-terminated.
    Tick limit = 0;
    uint32_t rate = 0;
    bool tiny_cache = false, small_pool = false, capture = false, linked = false;
    std::array<Event, kMaxEvents> events{};
    unsigned count = 0;
};

// Where a run stands in the script, which a run resumed from a state takes
// over from the run that saved it: the next key event, and for the frame
// and text ends the frames completed, the VBlank the run heads for and the
// text of the last frames.
struct Progress {
    unsigned next = 0;
    uint32_t frame = 0;
    Tick vblank = 0;  // 0 before it is chosen.
    unsigned unchanged = 0;
    std::array<uint8_t, kTextBytes> previous{};
};

// The loop of every run: the script's keys, the quanta, the end condition
// and the PCM drained between quanta, outside the wall time.
struct Run {
    System& system;
    const Script& script;
    PcmDigest& pcm;
    const std::atomic<bool>& cancel;
    std::atomic<uint32_t>& progress;
    Lockstep* lockstep = nullptr;
    Progress at{};
    std::array<uint8_t, kTextBytes> text{};
    int64_t untimed = 0;  // Draining, in us.
    // A run that never waits for the renderer would starve the idle task
    // of its core and its watchdog: it yields a tick now and then, as the
    // GB runs do.
    int64_t yield_at = esp_timer_get_time() + 50'000;

    // Whether a row of the console text holds one of the markers.
    bool marked() const {
        for (const char* marker = script.markers.data(); *marker;) {
            const char* end = std::strchr(marker, '\n');
            const size_t length = end ? static_cast<size_t>(end - marker) : std::strlen(marker);
            for (unsigned row = 0; row < kTextRows && length && length <= kTextColumns; ++row) {
                const auto* line = reinterpret_cast<const char*>(text.data()) + row * kTextColumns;
                for (size_t from = 0; from + length <= kTextColumns; ++from)
                    if (!std::memcmp(line + from, marker, length)) return true;
            }
            if (!end) break;
            marker = end + 1;
        }
        return false;
    }
    Tick apply() {
        for (; at.next < script.count && script.events[at.next].at <= system.now(); ++at.next) {
            system.set_buttons(script.events[at.next].keys);
            if (lockstep) lockstep->interpreter.set_buttons(script.events[at.next].keys);
        }
        return at.next < script.count ? script.events[at.next].at : kNever;
    }
    // Runs to `until` in quanta; false when cancelled, at the idle loop or
    // at a lockstep difference. Quanta end at each multiple of 65536 ticks,
    // wherever the run stopped before, and the idle loop is checked where a
    // quantum passed one, or the limit: the first instruction boundary at
    // or after it, whatever the quantum's target was, so that a run resumed
    // from a state, or one that steps to a save point, checks at the times
    // an uninterrupted run does.
    bool to(Tick until, uint8_t& reason) {
        while (system.now() < until) {
            if (cancel.load(std::memory_order_relaxed)) return false;
            const Tick event = apply();
            const Tick boundary = (system.now() | 0xffff) + 1;
            const Tick target = std::min({until, boundary, event});
            if (lockstep) lockstep->begin(target);
            system.run_until(target);
            progress.fetch_add(1, std::memory_order_relaxed);
            if (lockstep && !lockstep->end_quantum()) {
                reason = 5;
                return false;
            }
            if (script.end == 1 && system.now() >= std::min(boundary, script.limit) && system.idle_loop()) {
                reason = 1;
                return false;
            }
            const auto drain = esp_timer_get_time();
            pcm.drain(system);
            if (lockstep) lockstep->pcm->drain(lockstep->interpreter);
            const auto now = esp_timer_get_time();
            untimed += now - drain;
            if (now >= yield_at) {
                vTaskDelay(1);
                yield_at = esp_timer_get_time() + 50'000;
            }
        }
        return true;
    }
    // Runs on to the script's end, or to the first instruction boundary at
    // or after `pause`; returns RESULT's reason, 0 when it paused there.
    uint8_t run(Tick pause = kNever) {
        uint8_t reason = 2;
        if (script.end == 1) {
            if (!to(std::min(script.limit, pause), reason)) return reason;
            return pause < script.limit ? 0 : reason;
        }
        for (;;) {
            // A pause that stops past the VBlank leaves it chosen, so that
            // the frame still ends where an uninterrupted run ends it.
            if (!at.vblank) at.vblank = system.memory.ppu.next_vblank(system.now());
            const Tick until = std::min(at.vblank, script.limit);
            if (pause < until) return to(pause, reason) ? 0 : reason;
            if (!to(until, reason) || at.vblank > script.limit) return reason;
            at.vblank = 0;
            ++at.frame;
            if (script.end == 2 && at.frame == script.frames) return 3;
            if (script.end == 3) {
                console_text(system, text);
                at.unchanged = text == at.previous ? at.unchanged + 1 : 0;
                at.previous = text;
                if (at.unchanged >= 2 && marked()) return 3;
            }
        }
    }
};

struct Hasher {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool valid = false;
    static void observe(void* context, std::span<const uint8_t> bytes) {
        auto& self = *static_cast<Hasher*>(context);
        self.valid = self.valid && psa_hash_update(&self.operation, bytes.data(), bytes.size()) == PSA_SUCCESS;
    }
};

// The ROM cache's budget: a ROM that fits stays whole.
constexpr size_t kCacheBudget = size_t{8} << 20;

gba::Backend backend(uint8_t value) { return value ? gba::Backend::jit : gba::Backend::interpreter; }

// The JIT's capacity in the script.
gba::Arm7JitCapacity capacity(const Script& script) {
    return script.small_pool ? gba::Arm7JitCapacity{16 * 1024, 64} : gba::Arm7JitCapacity{};
}

// The block the JIT's code pool lives in, set aside before the system is
// created, as the product does; it must outlive the system.
platform::ExecutableMemory reserve_jit(const Script& script) {
    return platform::allocate_executable(gba::Arm7Jit::reserved_bytes(capacity(script)));
}

// `reserved`, for the JIT, is the block reserve_jit() set aside before the
// system was created.
bool start(System& system, const Script& script, gba::RomCache& cache, gba::Backend backend,
           platform::ExecutableMemory* reserved = nullptr) {
    system.set_jit_capacity(capacity(script));
    const bool initialized =
        system.initialize() && system.load(std::span<const uint8_t, gba::kBiosBytes>(bios, gba::kBiosBytes), cache);
    const bool ready = initialized && system.select_backend(backend, reserved);
    if (!ready)
        platform::log(platform::LogLevel::error, "gba-rom", "%s",
                      !initialized ? "Cannot initialize the system" : "Cannot start the JIT");
    if (ready) system.set_audio_rate(script.rate);
    return ready;
}

std::unique_ptr<System> new_system() {
    auto system = std::unique_ptr<System>(new (std::nothrow) System);
    if (!system) platform::log(platform::LogLevel::error, "gba-rom", "Cannot allocate the system");
    return system;
}

struct Job {
    std::span<const uint8_t> digest;  // The ROM's expected SHA256.
    char path[272]{};
    const Script* script = nullptr;
    uint8_t mode = 0;
    std::array<uint8_t, 2> backends{};
    unsigned count = 0;
    std::array<Result, 2> results;
    std::array<PcmDigest, 2> pcm;
    // The second for the JIT in a concurrent run. The paired runs' cache
    // stays for the save points.
    std::array<std::unique_ptr<gba::RomCache>, 2> caches;
    std::atomic<uint32_t> progress{0};
    std::atomic<bool> cancel{false};
    EventGroupHandle_t events = nullptr;
    std::array<const char*, 2> failures{};
    bool diverged = false;
    Diff diff;
};
// Event bits: a run's end (bit 0, and bit 1 for the second concurrent
// run), a concurrent run's cache ready (bits 2 and 3), and the save points'.
constexpr EventBits_t kDone = 1, kReady = 4, kStateDone = 16, kRequest = 32, kReply = 64;

// Reads and hashes the ROM into a cache of the job's.
bool open_cache(Job& job, unsigned index) {
    auto& cache = job.caches[index];
    cache.reset(new (std::nothrow) gba::RomCache);
    Hasher hasher;
    hasher.valid = psa_hash_setup(&hasher.operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    const bool opened = cache && cache->open(job.path, kCacheBudget, &Hasher::observe, &hasher);
    Digest digest{};
    size_t written = 0;
    hasher.valid = hasher.valid && psa_hash_finish(&hasher.operation, digest.data(), digest.size(), &written) ==
                                       PSA_SUCCESS && written == 32;
    if (!hasher.valid) psa_hash_abort(&hasher.operation);
    if (!opened) job.failures[index] = "Cannot read the ROM into the ROM cache";
    else if (!hasher.valid || !std::equal(digest.begin(), digest.end(), job.digest.begin()))
        job.failures[index] = "ROM content does not match the requested manifest entry";
    return !job.failures[index];
}

// One backend's run in a system of its own. `reserved`: the JIT's block,
// when the caller took it, which run_backend() then frees.
void run_backend(Job& job, gba::RomCache& cache, unsigned index, platform::ExecutableMemory reserved = {}) {
    const auto& script = *job.script;
    auto& r = job.results[index];
    // The cache first: the first time it pages, it starts its reader, whose
    // stack in internal RAM lives as long as the cache and would otherwise
    // land past this system's memory, in the middle of the space the JIT's
    // code pool needs once the system is gone.
    if (script.tiny_cache) cache.set_frames(gba::RomCache::kMinFrames);
    if (r.backend && !reserved.words) reserved = reserve_jit(script);
    auto system = new_system();
    const bool ready = system && start(*system, script, cache, backend(r.backend), &reserved);
    if (ready) {
#ifdef GBX_PROFILER
        // A concurrent run is not profiled: both cores emulate.
        const bool profiled = job.mode != 4;
        if (profiled) {
            r.profile.start(system->jit());
            r.running.store(true, std::memory_order_relaxed);
        }
#endif
        const auto begin = esp_timer_get_time();
        Run run{*system, script, job.pcm[index], job.cancel, job.progress};
        r.reason = run.run();
        system->memory.ppu.finish();
        system->finish_audio();
        r.wall = static_cast<uint64_t>(esp_timer_get_time() - begin - run.untimed);
#ifdef GBX_PROFILER
        if (profiled) {
            r.running.store(false, std::memory_order_relaxed);
            r.profile.stop();
        }
#endif
        job.pcm[index].finish(*system, r);
        summarize(*system, r);
    } else {
        r.capture_frame = false;
    }
    // The code pool is retired on its owner core.
    system.reset();
    platform::free_executable(reserved);
}

// Both systems at once, see Lockstep; a memory difference at a quantum's
// end runs once more with the differing regions compared at each turn.
void run_lockstep(Job& job, gba::RomCache& cache) {
    const auto& script = *job.script;
    if (script.tiny_cache) cache.set_frames(gba::RomCache::kMinFrames);
    // The JIT's system and its code pool first, as in a paired run.
    auto reserved = reserve_jit(script);
    auto jit = new_system();
    const bool jit_ready = jit && start(*jit, script, cache, gba::Backend::jit, &reserved);
    std::unique_ptr<System> interpreter;
    if (jit_ready) interpreter = new_system();
    if (!interpreter || !start(*interpreter, script, cache, gba::Backend::interpreter)) {
        for (auto& r : job.results) r.capture_frame = false;
        interpreter.reset();
        jit.reset();
        platform::free_executable(reserved);
        return;
    }
    const auto begin = esp_timer_get_time();
    uint64_t detail = UINT64_MAX;
    uint8_t regions = 0, reason = 2;
    int64_t untimed = 0;
    for (bool replay = false;; replay = true) {
        if (replay) {
            for (auto* system : {interpreter.get(), jit.get()})
                system->load(std::span<const uint8_t, gba::kBiosBytes>(bios, gba::kBiosBytes), cache);
            job.pcm = {};
        }
        Lockstep lockstep(*jit, *interpreter);
        lockstep.pcm = &job.pcm[0];
        lockstep.detail = detail;
        lockstep.regions = regions;
        jit->jit()->observe(&Lockstep::observe, &lockstep, script.linked);
        Run run{*jit, script, job.pcm[1], job.cancel, job.progress};
        run.lockstep = &lockstep;
        reason = run.run();
        untimed += run.untimed;
        jit->jit()->observe(nullptr, nullptr);
        const uint8_t memory = lockstep.differing & ~(1u << 4);
        if (!replay && memory) {
            detail = lockstep.quantum;
            regions = memory;
            continue;
        }
        job.diverged = lockstep.diverged;
        job.diff = lockstep.diff;
        break;
    }
    for (auto* system : {interpreter.get(), jit.get()}) {
        system->memory.ppu.finish();
        system->finish_audio();
    }
    const auto wall = static_cast<uint64_t>(esp_timer_get_time() - begin - untimed);
    for (unsigned i = 0; i < 2; ++i) {
        System& system = i ? *jit : *interpreter;
        auto& r = job.results[i];
        r.reason = reason;
        r.wall = wall;
        job.pcm[i].finish(system, r);
        summarize(system, r);
    }
    interpreter.reset();
    jit.reset();
    platform::free_executable(reserved);
}

// Runs on core 1, so that the renderer each system starts goes to core 0:
// the ROM is read and hashed into one cache, which the backends' systems
// use in turn.
void worker(void* context) {
    auto& job = *static_cast<Job*>(context);
    if (open_cache(job, 0)) {
        if (job.mode == 3) run_lockstep(job, *job.caches[0]);
        else
            for (unsigned i = 0; i < job.count; ++i) run_backend(job, *job.caches[0], i);
    }
    xEventGroupSetBits(job.events, kDone);
    vTaskDelete(nullptr);
}

// A concurrent run (mode 4): one backend on each core, with the renderer
// and mixer of each on the other, each with a cache of its own. Both start
// once both caches are ready.
struct Half {
    Job* job = nullptr;
    unsigned index = 0;
};
void concurrent_worker(void* context) {
    const auto& half = *static_cast<Half*>(context);
    auto& job = *half.job;
    // The JIT's block before either system is created: both take internal
    // RAM, and the other one starts when both are ready.
    auto reserved = job.results[half.index].backend ? reserve_jit(*job.script) : platform::ExecutableMemory{};
    const bool opened = open_cache(job, half.index);
    xEventGroupSetBits(job.events, kReady << half.index);
    xEventGroupWaitBits(job.events, kReady | kReady << 1, pdFALSE, pdTRUE, portMAX_DELAY);
    if (opened && !job.failures[half.index ^ 1]) {
        run_backend(job, *job.caches[half.index], half.index, reserved);
    } else {
        platform::free_executable(reserved);
        job.results[half.index].capture_frame = false;
    }
    xEventGroupSetBits(job.events, kDone << half.index);
    vTaskDelete(nullptr);
}

// Instant saves, see the save point and STATE RESULT in gba_rom_test.h. A
// save point runs once per save backend, in a system of its own on core 1:
// to the target, then one instruction boundary at a time until the
// conditions hold, where the owner captures the state. The transport task
// on core 0 encodes it, and decodes it again before each load, as the
// product's other task does, while the owner waits so that the timings stay
// apart. Each load resumes in the save system on one backend, the save
// backend first, and runs on to the end of the uninterrupted runs it is
// compared with.
constexpr unsigned kMaxSavePoints = 8;
constexpr uint16_t kConditions = 0x7f;
constexpr Tick kFrameCycles = 280'896;
struct SavePoint { Tick at; uint32_t window; uint16_t conditions; uint8_t combos; };
enum class StateStatus : uint8_t { resumed, ended, not_met, capture, encoding, decoding, restore, unavailable };
// The conditions in `mask` that hold now.
uint16_t conditions(System& s, uint16_t mask) {
    auto& m = s.memory;
    uint16_t met = 0;
    if ((mask & 1) && m.repeat_dma_moved()) met |= 1;
    if ((mask & 2) && m.halted) met |= 2;
    if ((mask & 4) && m.ppu.timed_writes()) met |= 4;
    if ((mask & 8) && m.fifo_queued()) met |= 8;
    if ((mask & 16) && m.prefetch_held()) met |= 16;
    if ((mask & 32) && m.backup_busy()) met |= 32;
    if ((mask & 64) && m.rtc_busy()) met |= 64;
    return met;
}
struct StateLoad {
    uint8_t backend = 0, decode = 0;
    StateStatus status = StateStatus::unavailable;
    uint32_t decode_us = 0, restore_us = 0;
    std::array<uint32_t, 2> frame_us{}, compiled{}, installed{};
    Result result;
};
struct StateJob {
    gba::RomCache* cache = nullptr;
    const Script* script = nullptr;
    uint8_t index = 0, save = 0;
    SavePoint point{};
    EventGroupHandle_t events = nullptr;
    // The transport task works on the snapshots and the buffer only between
    // kRequest and kReply, while the worker waits.
    bool decode = false;
    gba::Snapshot saved, loaded;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buffer{nullptr, &heap_caps_free};
    size_t buffer_size = 0, encoded = 0;
    uint8_t decode_status = 0;
    uint32_t capture_us = 0, encode_us = 0, decode_us = 0;
    uint16_t met = 0;
    Tick saved_at = 0;
    uint64_t dropped = 0;
    Progress at;           // Where the save run stood in the script.
    PcmDigest prefix, pcm;  // Up to the save, and of the run going on.
    std::array<StateLoad, 2> loads;
    unsigned load_count = 0;
    std::atomic<uint32_t> progress{0};
    std::atomic<bool> cancel{false};
    platform::ExecutableMemory reserved{};  // For the JIT, see reserve_jit().
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
        job.encode_us = static_cast<uint32_t>(esp_timer_get_time() - start);
    } else {
        job.decode_status = static_cast<uint8_t>(job.loaded.decode({job.buffer.get(), job.encoded}));
        job.decode_us = static_cast<uint32_t>(esp_timer_get_time() - start);
    }
}
// Runs to the save point and captures and encodes the state there.
StateStatus save(StateJob& job, System& s) {
    const auto& point = job.point;
    const auto& script = *job.script;
    Run run{s, script, job.pcm, job.cancel, job.progress};
    uint8_t reason = run.run(point.at);
    auto status = reason ? StateStatus::ended : StateStatus::resumed;
    const Tick last = std::min(point.at + point.window, script.limit);
    while (status == StateStatus::resumed && conditions(s, point.conditions) != point.conditions) {
        if (s.now() >= last) status = StateStatus::not_met;
        else if ((reason = run.run(s.now() + 1))) status = StateStatus::ended;
    }
    job.saved_at = s.now();
    job.at = run.at;
    job.met = conditions(s, kConditions);
    for (auto& out : job.loads) out.result.reason = reason ? reason : 2;
    if (status != StateStatus::resumed) return status;
    const auto start = esp_timer_get_time();
    const bool captured = s.capture(job.saved);
    job.capture_us = static_cast<uint32_t>(esp_timer_get_time() - start);
    if (!captured) return StateStatus::capture;
    // capture() waited for the sound handed to the mixer: this is all of it
    // up to the save.
    job.pcm.drain(s);
    job.prefix = job.pcm;
    job.dropped = s.audio_dropped();
    ask(job, false);
    return job.encoded ? StateStatus::resumed : StateStatus::encoding;
}
void load(StateJob& job, System& s, StateLoad& out) {
    s.reset();
    // A refused load reports the system it was refused in.
    const auto refuse = [&](StateStatus status) {
        out.status = status;
        out.result.capture_frame = false;
        summarize(s, out.result);
    };
    // Selecting a backend clears the JIT's code, so the same one is kept.
    if (s.backend() != backend(out.backend) && !s.select_backend(backend(out.backend), &job.reserved))
        return refuse(StateStatus::unavailable);
    ask(job, true);
    out.decode = job.decode_status;
    out.decode_us = job.decode_us;
    if (out.decode != static_cast<uint8_t>(gba::Snapshot::Status::ok)) return refuse(StateStatus::decoding);
    auto start = esp_timer_get_time();
    const bool restored = s.restore(job.loaded);
    out.restore_us = static_cast<uint32_t>(esp_timer_get_time() - start);
    if (!restored) return refuse(StateStatus::restore);
    out.status = StateStatus::resumed;
    job.pcm = job.prefix;
    Run run{s, *job.script, job.pcm, job.cancel, job.progress};
    run.at = job.at;
    const auto* jit = s.jit();
    const Tick from = s.now();
    start = esp_timer_get_time();
    uint8_t reason = 0;
    for (unsigned k = 0; k < 2 && !reason; ++k) {
        const uint32_t compiled = jit ? jit->compiled_blocks() : 0, installed = jit ? jit->restored_blocks() : 0;
        const auto begin = esp_timer_get_time();
        const auto untimed = run.untimed;
        reason = run.run(from + (k + 1) * kFrameCycles);
        if (!reason) {
            out.frame_us[k] = static_cast<uint32_t>(esp_timer_get_time() - begin - (run.untimed - untimed));
            out.compiled[k] = jit ? jit->compiled_blocks() - compiled : 0;
            out.installed[k] = jit ? jit->restored_blocks() - installed : 0;
        }
    }
    if (!reason) reason = run.run();
    s.memory.ppu.finish();
    s.finish_audio();
    out.result.wall = static_cast<uint64_t>(esp_timer_get_time() - start - run.untimed);
    out.result.reason = reason;
    job.pcm.finish(s, out.result);
    out.result.pcm_dropped += job.dropped;
    summarize(s, out.result);
}
void state_worker(void* context) {
    auto& job = *static_cast<StateJob*>(context);
    const auto& script = *job.script;
    if (script.tiny_cache) job.cache->set_frames(gba::RomCache::kMinFrames);
    // Held until a load starts the JIT, if the save ran on the interpreter.
    job.reserved = reserve_jit(script);
    auto system = new_system();
    bool ready = system && start(*system, script, *job.cache, backend(job.save), &job.reserved);
    if (ready) {
        // Allocated at load, as the product does, so that saving and
        // loading allocate nothing.
        ready = job.saved.allocate() && job.loaded.allocate();
        job.buffer_size = job.saved.max_encoded();
        if (ready)
            job.buffer.reset(static_cast<uint8_t*>(heap_caps_malloc(job.buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        ready = ready && job.buffer;
    }
    auto status = StateStatus::unavailable;
    if (ready) status = save(job, *system);
    if (status == StateStatus::resumed) {
        for (unsigned i = 0; i < job.load_count; ++i) load(job, *system, job.loads[i]);
    } else {
        // Every requested load reports where the save run stopped.
        for (unsigned i = 0; i < job.load_count; ++i) {
            auto& out = job.loads[i];
            out.status = status;
            out.result.capture_frame = false;
            if (ready) summarize(*system, out.result);
        }
    }
    // The code pool is retired on its owner core.
    system.reset();
    platform::free_executable(job.reserved);
    xEventGroupSetBits(job.events, kStateDone);
    vTaskDelete(nullptr);
}
bool send_state(const StateJob& job, const StateLoad& load, uint32_t id) {
    Packet out;
    out.header(4, load.backend, id);
    out.u8(job.index);
    out.u8(job.save);
    out.u8(static_cast<uint8_t>(load.status));
    out.u8(load.decode);
    out.u16(job.met);
    out.u16(0);
    out.u64(job.saved_at);
    out.u32(static_cast<uint32_t>(job.encoded));
    out.u32(job.capture_us);
    out.u32(job.encode_us);
    out.u32(load.decode_us);
    out.u32(load.restore_us);
    for (const auto& values : {load.frame_us, load.compiled, load.installed})
        for (auto v : values) out.u32(v);
    result_body(out, load.result);
    return out.send();
}
// Runs every save point and sends its STATE RESULTs, and a STATE FRAME for
// each resumed frame unlike the uninterrupted one, references[backend].
bool run_states(gba::RomCache& cache, const Script& script, std::span<const SavePoint> points,
                std::array<const Result*, 2> references, EventGroupHandle_t events, uint32_t id) {
    struct Release {
        void operator()(StateJob* job) const {
            job->~StateJob();
            heap_caps_free(job);
        }
    };
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> frames{nullptr, &heap_caps_free};
    if (script.capture) {
        frames.reset(static_cast<uint8_t*>(heap_caps_malloc(2 * kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!frames) return fail("Cannot allocate GBA frames");
    }
    for (unsigned index = 0; index < points.size(); ++index)
        for (uint8_t save = 0; save < 2; ++save) {
            const auto& point = points[index];
            // Bit 0 of the pair loads on INT, bit 1 on JIT.
            const unsigned loads = (point.combos >> save * 2) & 3;
            if (!loads) continue;
            // In PSRAM, which leaves internal RAM to the JIT code pool.
            void* storage =
                heap_caps_aligned_alloc(alignof(StateJob), sizeof(StateJob), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!storage) return fail("Cannot allocate a save point");
            std::unique_ptr<StateJob, Release> job(new (storage) StateJob);
            job->cache = &cache;
            job->script = &script;
            job->index = static_cast<uint8_t>(index);
            job->save = save;
            job->point = point;
            job->events = events;
            for (const uint8_t target : {save, static_cast<uint8_t>(save ^ 1)}) {
                if (!(loads & (1u << target))) continue;
                auto& out = job->loads[job->load_count];
                out.backend = target;
                out.result.backend = target;
                out.result.capture_frame = script.capture;
                out.result.frame = frames ? frames.get() + job->load_count * kFrameBytes : nullptr;
                ++job->load_count;
            }
            xEventGroupClearBits(events, kStateDone | kRequest | kReply);
            if (xTaskCreatePinnedToCore(state_worker, "gba-state", 8192, job.get(), 5, nullptr, 1) != pdPASS)
                return fail("Cannot start a save point");
            bool ok = true;
            uint32_t last_progress = 0;
            for (;;) {
                const auto bits =
                    xEventGroupWaitBits(events, kStateDone | kRequest, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
                if (bits & kRequest) {
                    xEventGroupClearBits(events, kRequest);
                    serve(*job);
                    xEventGroupSetBits(events, kReply);
                    continue;
                }
                if (bits & kStateDone) break;
                const auto progress = job->progress.load(std::memory_order_relaxed);
                if (!ok || progress == last_progress) continue;
                Packet out;
                out.header(2, 2, id);
                out.u64(progress);
                ok = out.send();
                last_progress = progress;
                if (!ok) job->cancel.store(true, std::memory_order_relaxed);
            }
            for (unsigned i = 0; ok && i < job->load_count; ++i) {
                const auto& resumed = job->loads[i];
                const auto& r = resumed.result;
                const auto& reference = *references[resumed.backend];
                ok = send_state(*job, resumed, id);
                // The PC has the uninterrupted frame; another one is sent.
                if (ok && r.capture_frame && r.frame_size &&
                    (!reference.frame_size || r.frame_id != reference.frame_id ||
                     r.frame_digest != reference.frame_digest))
                    ok = send_frame(r, id, resumed.backend, job->index, job->save);
            }
            if (!ok) return false;
        }
    return true;
}

struct Release {
    void operator()(void* memory) const { heap_caps_free(memory); }
};
// Jobs live in PSRAM.
struct ReleaseJob {
    void operator()(Job* job) const {
        job->~Job();
        heap_caps_free(job);
    }
};
// The run's event group, deleted after every task that uses it ended.
struct Events {
    EventGroupHandle_t handle = xEventGroupCreate();
    ~Events() {
        if (handle) vEventGroupDelete(handle);
    }
};

}  // namespace

std::array<uint8_t, 32> load_bios() {
    Digest digest{};
    auto* file = std::fopen("/sdcard/bios/gba_bios.bin", "rb");
    if (!file) return digest;
    const bool read = std::fread(bios, 1, gba::kBiosBytes, file) == gba::kBiosBytes && std::fgetc(file) == EOF &&
                      !std::ferror(file);
    std::fclose(file);
    bios_valid = read && hash({{bios, gba::kBiosBytes}}, digest);
    if (!bios_valid) digest = {};
    return digest;
}

bool run(std::span<const uint8_t> request) {
    constexpr size_t kHeader = 72;
    if (request.size() < kHeader || p::load16(request.data()) != kVersion || request[2] > 4 || request[3] < 1 ||
        request[3] > 3)
        return error("Invalid GBA ROM request or protocol version");
    if (!bios_valid) return error("No valid GBA BIOS on the card");
    const uint8_t mode = request[2];
    const uint32_t id = p::load32(request.data() + 4);
    auto script = std::unique_ptr<Script>(new (std::nothrow) Script);
    if (!script) return error("Cannot allocate the ROM script");
    script->end = request[3];
    script->limit = p::load32(request.data() + 8) | (Tick{p::load32(request.data() + 12)} << 32);
    const uint32_t size = p::load32(request.data() + 16);
    const unsigned path_size = p::load16(request.data() + 20), events = p::load16(request.data() + 22);
    script->frames = p::load32(request.data() + 24);
    const unsigned marker_size = p::load16(request.data() + 28);
    const uint8_t flags = request[30];
    script->tiny_cache = flags & 1;
    script->small_pool = flags & 2;
    script->capture = flags & 4;
    const bool card_root = flags & 8;
    script->linked = flags & 16;
    script->rate = p::load32(request.data() + 64);
    const unsigned saves = p::load16(request.data() + 68);
    const size_t points_at = kHeader + events * 12u, markers_at = points_at + saves * 16u,
                 path_at = markers_at + marker_size;
    if (!id || !script->limit || script->limit > 120 * gba::kCyclesPerSecond || !size ||
        size > gba::RomCache::kMaxBytes || events > kMaxEvents || path_size > 240 || flags > 31 || request[31] ||
        script->rate > 192000 || saves > kMaxSavePoints || p::load16(request.data() + 70) ||
        request.size() != path_at + path_size || marker_size >= script->markers.size() ||
        (script->end == 3) != (marker_size != 0) || (script->end == 2) != (script->frames != 0))
        return error("Invalid GBA ROM size, path, flags, end, rate or limit");
    // The save points compare with the paired runs on core 1.
    if (saves && mode != 0) return error("Save points require paired runs");
    std::memcpy(script->markers.data(), request.data() + markers_at, marker_size);
    if (std::strlen(script->markers.data()) != marker_size) return error("Invalid GBA ROM end markers");
    script->count = events;
    for (unsigned i = 0; i < events; ++i) {
        const uint8_t* e = request.data() + kHeader + i * 12;
        auto& event = script->events[i];
        event = {p::load32(e) | (Tick{p::load32(e + 4)} << 32), p::load16(e + 8)};
        if (event.at >= script->limit || (i && event.at < script->events[i - 1].at) || event.keys > 0x3ff ||
            p::load16(e + 10))
            return error("Invalid GBA ROM key event");
    }
    std::array<SavePoint, kMaxSavePoints> points{};
    unsigned loads = 0;
    for (unsigned i = 0; i < saves; ++i) {
        const uint8_t* s = request.data() + points_at + i * 16;
        auto& point = points[i];
        point = {p::load32(s) | (Tick{p::load32(s + 4)} << 32), p::load32(s + 8), p::load16(s + 12), s[14]};
        if (point.at >= script->limit || point.window > gba::kCyclesPerSecond || point.conditions > kConditions ||
            !point.combos || point.combos > 15 || s[15])
            return error("Invalid GBA ROM save point");
        loads += std::popcount(point.combos);
    }
    char relative[241]{};
    std::memcpy(relative, request.data() + path_at, path_size);
    if (std::strlen(relative) != path_size || std::strncmp(relative, "roms/", 5) || std::strstr(relative, "..") ||
        std::strchr(relative, '\\'))
        return error("Invalid GBA ROM path");
    // In PSRAM, which leaves internal RAM to the JIT code pool.
    void* storage = heap_caps_aligned_alloc(alignof(Job), sizeof(Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) return error("Cannot allocate the GBA ROM job");
    std::unique_ptr<Job, ReleaseJob> job(new (storage) Job);
    job->digest = request.subspan(32, 32);
    // Card-relative, as platform::files takes paths.
    std::snprintf(job->path, sizeof(job->path), card_root ? "%s" : "tests/gba/%s", relative);
    job->script = script.get();
    job->mode = mode;
    const bool both = mode == 0 || mode == 3 || mode == 4;
    job->count = both ? 2 : 1;
    job->backends = both ? std::array<uint8_t, 2>{0, 1} : std::array<uint8_t, 2>{static_cast<uint8_t>(mode - 1), 0};
    std::unique_ptr<uint8_t, Release> frames;
    if (script->capture) {
        frames.reset(static_cast<uint8_t*>(heap_caps_malloc(2 * kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!frames) return error("Cannot allocate GBA frames");
    }
    for (unsigned i = 0; i < job->count; ++i) {
        auto& r = job->results[i];
        r.backend = job->backends[i];
        r.core = mode == 4 ? static_cast<uint8_t>(i) : 1;
        r.capture_frame = script->capture;
        r.frame = frames ? frames.get() + i * kFrameBytes : nullptr;
    }
    Events group;
    if (!group.handle) return error("Cannot allocate GBA ROM events");
    job->events = group.handle;
    bool ok = true;
#ifdef GBX_PROFILER
    // This task's core, 0, samples while each run goes on.
    if (mode != 4)
        for (unsigned i = 0; i < job->count; ++i)
            job->results[i].other_core.start(static_cast<const gb::Sm83Jit*>(nullptr), &job->results[i].running);
#endif
    std::array<Half, 2> halves{Half{job.get(), 0}, Half{job.get(), 1}};
    EventBits_t done = kDone;
    if (mode != 4) {
        if (xTaskCreatePinnedToCore(worker, "gba-rom", 8192, job.get(), 5, nullptr, 1) != pdPASS)
            return error("Cannot start the GBA ROM worker");
    } else {
        if (xTaskCreatePinnedToCore(concurrent_worker, "gba-int", 8192, &halves[0], 5, nullptr, 0) != pdPASS)
            return error("Cannot start the GBA ROM workers");
        done = kDone | kDone << 1;
        if (xTaskCreatePinnedToCore(concurrent_worker, "gba-jit", 8192, &halves[1], 5, nullptr, 1) != pdPASS) {
            // The first one goes on alone and gives up.
            job->failures[1] = "Cannot start the GBA ROM workers";
            xEventGroupSetBits(group.handle, kReady << 1 | kDone << 1);
        }
    }
    uint32_t last_progress = 0;
    while ((xEventGroupWaitBits(group.handle, done, pdFALSE, pdTRUE, pdMS_TO_TICKS(500)) & done) != done) {
        const auto progress = job->progress.load(std::memory_order_relaxed);
        if (!ok || progress == last_progress) continue;
        Packet out;
        out.header(2, 2, id);
        out.u64(progress);
        ok = out.send();
        last_progress = progress;
        if (!ok) job->cancel.store(true, std::memory_order_relaxed);
    }
    for (const char* failure : job->failures)
        if (failure) return error(failure);
    if (ok && job->diverged) ok = send_diff(job->diff, id);
    for (unsigned i = 0; ok && i < job->count; ++i) ok = send_result(job->results[i], id);
#ifdef GBX_PROFILER
    if (mode != 4) {
        for (unsigned i = 0; i < job->count; ++i) job->results[i].other_core.stop();
        for (unsigned i = 0; ok && i < job->count; ++i) {
            ok = job->results[i].profile.send(job->results[i].backend, 1);
            if (ok) ok = job->results[i].other_core.send(job->results[i].backend, 0);
        }
    }
#endif
    if (ok && script->capture) {
        const auto& a = job->results[0];
        const auto& b = job->results[1];
        const bool shared = job->count == 2 && a.frame_size && b.frame_size && a.frame_id == b.frame_id &&
                            a.frame_digest == b.frame_digest;
        if (shared) ok = send_frame(a, id, 2);
        else
            for (unsigned i = 0; ok && i < job->count; ++i)
                if (job->results[i].frame_size) ok = send_frame(job->results[i], id, job->results[i].backend);
    }
    if (ok && saves)
        ok = run_states(*job->caches[0], *script, std::span(points).first(saves),
                        {&job->results[0], &job->results[1]}, group.handle, id);
    const unsigned count = job->count + loads;
    if (!ok || !transport::finish()) return false;
    uint8_t done_payload[8];
    p::store16(done_payload, kVersion);
    p::store16(done_payload + 2, static_cast<uint16_t>(count));
    p::store32(done_payload + 4, id);
    return transport::send(p::Kind::done, done_payload);
}

}  // namespace gbx::test::gba_rom_test

