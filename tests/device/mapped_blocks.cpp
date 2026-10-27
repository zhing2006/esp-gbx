#include "mapped_blocks.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>

#include "gbx/gb/system.h"

namespace gbx::test {
namespace {
using namespace gb;
struct Event {
    Memory* memory;
    Tick observed = 0;
    uint16_t address = 0xc000;
    uint8_t value = 0x40;
    static void fire(void* context) {
        auto& event = *static_cast<Event*>(context);
        event.observed = event.memory->clock.now;
        event.memory->poke(event.address, event.value);
    }
};
void start(System& s, uint16_t pc) {
    s.reset(); s.memory.clock.reset(); s.cpu = {}; s.cpu.pc = pc; s.cpu.sp = 0xfffe;
}
bool instructions(System& s, unsigned count) {
    while (count) {
        const auto r = s.step_block(kTicksPerSecond, count);
        if (!r.instructions || r.instructions > count || r.fallback != JitFallback::none) return false;
        count -= r.instructions;
    }
    return true;
}
uint32_t event_access(System& s) {
    uint32_t mask = 0;
    // LD A,(HL) samples data at tick 14. A change at 10 or 14 must be
    // observed; at 15 it must not retroactively alter the read. All three
    // events occur during the two-M-cycle instruction, not at its boundary.
    for (unsigned i = 0; i < 3; ++i) {
        start(s, 0x100); s.cpu.set_hl(0xc000); s.memory.poke(0xc000, 0x12);
        Event event{&s.memory};
        const Tick when = i == 0 ? 10 : i == 1 ? 14 : 15;
        s.memory.clock.schedule(1, when, Event::fire, &event);
        const auto r = s.step_block(kTicksPerSecond);
        if (r.instructions == 1 && r.mcycles == 2 && r.fallback == JitFallback::none &&
            s.cpu.pc == 0x101 && s.cpu.a == (i == 2 ? 0x12 : 0x40) &&
            s.memory.clock.now == 16 && event.observed == when) mask |= 1u << i;
    }
    return mask;
}
uint32_t wram_rewrite(System& s) {
    start(s, 0xc000);
    constexpr uint8_t code[]{0x3e,0x12,0x18,0xfc};  // LD A,12; JR -4.
    for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc000 + i, code[i]);
    uint32_t mask = 0;
    if (instructions(s, 2) && s.cpu.a == 0x12 && s.cpu.pc == 0xc000) mask |= 1;
    const auto invalidations = s.jit() ? s.jit()->invalidations() : 0;
    s.memory.write(0xc000, 0x06); s.memory.write(0xc001, 0x34);  // LD B,34.
    if (instructions(s, 2) && s.cpu.b == 0x34 && s.cpu.a == 0x12) mask |= 2;
    // A refetch may retire the entry and finish via a one-instruction
    // continuation. Re-enter before the echo write so both rewrites
    // actually have a cached two-instruction block to invalidate.
    const bool recached = instructions(s, 2) && s.cpu.b == 0x34 && s.cpu.pc == 0xc000;
    s.memory.write(0xe000, 0x0e); s.memory.write(0xe001, 0x56);  // Echo alias: LD C,56.
    if (instructions(s, 2) && s.cpu.c == 0x56 && s.cpu.b == 0x34) mask |= 4;
    if (recached && (!s.jit() || s.jit()->invalidations() >= invalidations + 2)) mask |= 8;
    return mask;
}
uint32_t hram_self_modify(System& s) {
    start(s, 0xff80);
    // Change the not-yet-fetched LD A,34 into LD B,34 within this very block.
    constexpr uint8_t code[]{0x3e,0x06,0xea,0x85,0xff,0x3e,0x34,0x18,0xf7};
    for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xff80 + i, code[i]);
    const bool ok = instructions(s, 4);
    uint32_t mask = ok ? 1 : 0;
    if (s.cpu.a == 6 && s.cpu.b == 0x34 && s.cpu.pc == 0xff80) mask |= 2;
    if (s.memory.clock.now == 88) mask |= 4;
    if (!s.jit() || s.jit()->invalidations()) mask |= 8;
    return mask;
}
uint32_t fetched_opcode(System& s) {
    uint32_t mask = 0;
    Event event{&s.memory};
    start(s, 0xff30); s.cpu.a = 0x3c;
    constexpr uint8_t code[]{0xea,0x33,0xff,0x00};
    for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xff30 + i, code[i]);
    if (instructions(s, 2) && s.cpu.a == 0x3d && s.cpu.pc == 0xff34 && s.memory.clock.now == 40) mask |= 1;
    start(s, 0xc000);
    event = {&s.memory, 0, 0xc000, 0x40};
    s.memory.clock.schedule(1, 4, Event::fire, &event);
    if (s.step_block(kTicksPerSecond, 1).exit == JitExit::breakpoint) mask |= 2;
    start(s, 0xc000); s.memory.poke(0xc000, 0x40); s.cpu.a = 1;
    event = {&s.memory, 0, 0xc000, 0x3c};
    s.memory.clock.schedule(1, 4, Event::fire, &event);
    if (s.step_block(kTicksPerSecond, 1).exit != JitExit::breakpoint && s.cpu.a == 2) mask |= 4;
    return mask;
}
uint32_t block_boundaries(System& s) {
    uint32_t mask = 0;
    // A mapping change after the first CB byte must affect the second fetch,
    // even though the replacement page allows direct reads again.
    struct Mapping {
        Memory* memory;
        std::array<uint8_t, 128> bytes{};
        static void change(void* context) {
            auto& map = *static_cast<Mapping*>(context);
            map.memory->pages[0x180 >> 7].read = map.bytes.data();
        }
    } map{&s.memory};
    start(s, 0x180); s.cpu.b = 0x80;
    map.bytes[0] = 0xcb; map.bytes[1] = 0x08;
    s.memory.clock.schedule(1, 7, Mapping::change, &map);
    if (instructions(s, 1) && s.cpu.b == 0x40 && s.cpu.pc == 0x182 && s.memory.clock.now == 16) mask |= 1;
    start(s, 0x100); s.cpu.set_hl(0xc000); s.memory.poke(0xc000, 0x41);
    s.memory.clock.now = (Tick{1} << 32) - 8;
    const auto start_time = s.memory.clock.now;
    const auto first = s.step_block(start_time + 15);
    if (first.instructions == 1 && first.mcycles == 2 && s.cpu.a == 0x41 &&
        s.memory.clock.now == start_time + 16) mask |= 2;
    const auto stopped = s.step_block(s.memory.clock.now);
    if (!stopped.instructions && s.cpu.pc == 0x101 && s.memory.clock.now == start_time + 16) mask |= 4;
    // Delayed Z and pending EI must survive an opcode refetch with no duplicate
    // access. Expected behavior is ADD A,1; EI; INC A (replacement opcode).
    for (unsigned traced = 0; traced < 2; ++traced) {
        start(s, 0xc000); s.cpu.a = 0xff;
        constexpr uint8_t code[]{0xc6,1,0xfb,0};
        for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc000 + i, code[i]);
        std::array<TraceRecord, 8> records{};
        TraceBuffer trace; trace.reset(records); s.set_trace(traced ? &trace : nullptr);
        Event event{&s.memory, 0, 0xc003, 0x3c};
        s.memory.clock.schedule(1, 28, Event::fire, &event);
        bool good = instructions(s, 3) && s.cpu.pc == 0xc004 && s.cpu.a == 1 &&
            s.cpu.f == 0x10 && s.cpu.ime && !s.cpu.ei_pending && s.memory.clock.now == 32;
        if (traced) {
            TraceRecord record{}; unsigned count = 0;
            while (trace.pop(record)) {
                good &= count < 4 && record.time == 6 + count * 8 && record.address == 0xc000 + count &&
                    record.data == (count == 3 ? 0x3c : code[count]) && record.operation == 0;
                ++count;
            }
            good &= count == 4 && !trace.dropped();
        }
        if (good) mask |= 8u << traced;
        s.set_trace(nullptr);
    }
    bool suspended = true;
    unsigned mode = 2;
    for (const uint8_t opcode : {0x10, 0xd3}) {
        start(s, 0xc000); s.memory.poke(0xc000, opcode);
        const auto first = s.run_frame();
        const auto pc = s.cpu.pc;
        const auto again = s.run_frame();
        const auto until = s.run_until(s.memory.clock.now + 8);
        suspended &= first.end == RunEnd::suspended && first.instructions == 1 && first.time == 8 &&
            s.cpu.mode == static_cast<CpuMode>(mode++) &&
            pc == (opcode == 0x10 ? 0xc002 : 0xc001) && s.cpu.pc == pc &&
            again.end == RunEnd::suspended && !again.instructions && again.time == first.time &&
            until.end == RunEnd::suspended && !until.instructions && until.time == first.time;
    }
    if (suspended) mask |= 32;
    return mask;
}
uint32_t lazy_trace(System& s) {
    std::array<TraceRecord, 64> records{};
    TraceBuffer trace;
    uint32_t mask = 0;
    uint32_t normal_count = 0;
    for (unsigned traced = 0; traced < 2; ++traced) {
        start(s, 0x110);
        trace.reset(records); s.set_trace(traced ? &trace : nullptr);
        const auto result = s.step_block(kTicksPerSecond, 6);
        if (!traced) normal_count = result.instructions;
        if (result.instructions == 6 && result.fallback == JitFallback::none &&
            s.cpu.a == 2 && s.cpu.f == 0 && s.cpu.b == 0 && s.cpu.c == 1 &&
            s.cpu.pc == 0x119 && s.memory.clock.now == 72) mask |= 1u << traced;
        if (traced && result.instructions == normal_count) mask |= 4;
    }
    size_t count = 0; bool correct = true;
    TraceRecord record{};
    while (trace.pop(record)) {
        correct &= record.time == 6 + count * 8 && record.address == 0x110 + count && record.operation == 0;
        ++count;
    }
    if (correct && count == 9 && !trace.dropped()) mask |= 8;
    s.set_trace(nullptr);
    return mask;
}
uint32_t writable_refetch(System& s) {
    // ADD A,1 leaves lazy Z/C, then LD (HL),B replaces the next NOP with
    // INC C. The five accesses follow the SM83 instruction bus sequences.
    uint32_t mask = 0;
    constexpr uint8_t code[]{0xc6,1,0x70,0};
    auto prepare = [&] {
        start(s, 0xc000); s.cpu.a = 0xff; s.cpu.b = 0x0c; s.cpu.set_hl(0xc003);
        for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc000 + i, code[i]);
    };
    for (unsigned traced = 0; traced < 2; ++traced) {
        prepare();
        std::array<TraceRecord, 8> records{};
        TraceBuffer trace; trace.reset(records); s.set_trace(traced ? &trace : nullptr);
        if (instructions(s, 3) && s.cpu.pc == 0xc004 && s.cpu.a == 0 &&
            s.cpu.b == 0x0c && s.cpu.c == 1 && s.cpu.f == 0x10 && s.memory.clock.now == 40) mask |= 1u << traced;
        if (traced) {
            constexpr uint16_t addresses[]{0xc000,0xc001,0xc002,0xc003,0xc003};
            constexpr uint8_t values[]{0xc6,1,0x70,0x0c,0x0c};
            TraceRecord record{}; unsigned count = 0; bool good = true;
            while (trace.pop(record)) {
                good &= count < 5 && record.time == 6 + 8 * count &&
                    record.address == addresses[count] && record.data == values[count] &&
                    record.operation == (count == 3);
                ++count;
            }
            if (good && count == 5 && !trace.dropped()) mask |= 4;
        }
        s.set_trace(nullptr);
    }
    prepare();
    bool good = instructions(s, 3);
    s.cpu.pc = 0xc000; s.cpu.a = 0xff; good &= instructions(s, 3);
    const auto compiled = s.jit() ? s.jit()->compiled_blocks() : 0;
    s.cpu.pc = 0xc000; s.cpu.a = 0xff;
    if (good && instructions(s, 3) && s.cpu.c == 3 && s.memory.clock.now == 120 &&
        (!s.jit() || s.jit()->compiled_blocks() == compiled)) mask |= 8;
    return mask;
}
uint32_t writable_cb(System& s) {
    uint32_t mask = 0;
    for (unsigned traced = 0; traced < 2; ++traced) {
        start(s, 0xc070); s.cpu.b = 0x80;
        // Change CB 00 (RLC B) to CB 08 (RRC B) before its second-byte
        // fetch, across the 128-byte page boundary and without a device event.
        constexpr uint8_t code[]{0x3e,8,0xea,0x80,0xc0};
        for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc070 + i, code[i]);
        s.memory.poke(0xc07f, 0xcb);
        std::array<TraceRecord, 24> records{};
        TraceBuffer trace; trace.reset(records); s.set_trace(traced ? &trace : nullptr);
        if (instructions(s, 13) && s.cpu.pc == 0xc081 && s.cpu.b == 0x40 &&
            s.cpu.f == 0 && s.memory.clock.now == 144) mask |= 1u << traced;
        if (traced) {
            TraceRecord record{}; unsigned count = 0; bool good = true;
            while (trace.pop(record)) {
                good &= record.time == 6 + count * 8;
                if (count == 5) good &= record.address == 0xc080 && record.data == 8 && record.operation == 1;
                if (count >= 16) good &= record.address == 0xc07f + count - 16 &&
                    record.data == (count == 16 ? 0xcb : 8) && record.operation == 0;
                ++count;
            }
            if (good && count == 18 && !trace.dropped()) mask |= 4;
        }
        s.set_trace(nullptr);
    }
    return mask;
}
uint32_t batched_execution(System& s) {
    // INC B; NOP; JR -4 takes five M-cycles per iteration. Exercise the
    // normal run path across block/branch boundaries, including 64-bit time.
    constexpr uint8_t code[]{0x04, 0x00, 0x18, 0xfc};
    uint32_t mask = 0;
    for (unsigned traced = 0; traced < 2; ++traced) {
        start(s, 0xc000);
        for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc000 + i, code[i]);
        const Tick origin = (Tick{1} << 32) - 32;
        s.memory.clock.now = origin;
        std::array<TraceRecord, 16> records{};
        TraceBuffer trace; trace.reset(records); s.set_trace(traced ? &trace : nullptr);
        const auto result = s.run_until(origin + 79);
        bool good = result.end == RunEnd::target && result.instructions == 6 &&
            result.time == origin + 80 && s.cpu.pc == 0xc000 && s.cpu.b == 2 && !s.cpu.f;
        if (traced) {
            unsigned count = 0; TraceRecord record{};
            while (trace.pop(record)) {
                good &= count < 8 && record.time == origin + (count / 4) * 40 + (count % 4) * 8 + 6 &&
                    record.address == 0xc000 + count % 4 && record.data == code[count % 4] &&
                    record.operation == 0 && record.pc == 0xc000 + std::min(count % 4, 2u);
                ++count;
            }
            good &= count == 8 && !trace.dropped();
        }
        if (good) mask |= 1u << traced;
        s.set_trace(nullptr);
    }
    start(s, 0xc000);
    for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xc000 + i, code[i]);
    Event rewrite{&s.memory, 0, 0xc000, 0x0c};  // Replace INC B with INC C.
    s.memory.clock.schedule(1, 74, Event::fire, &rewrite);
    const auto result = s.run_until(120);
    if (result.end == RunEnd::target && result.instructions == 9 && result.time == 120 &&
        s.cpu.pc == 0xc000 && s.cpu.b == 2 && s.cpu.c == 1 && rewrite.observed == 74) mask |= 4;

    // Move the earliest event later, cancel a different event, then fire two
    // slots together. Rescheduling inside a callback must preserve their order.
    struct Chain {
        Scheduler* clock;
        std::array<Tick, 4> times{};
        std::array<unsigned, 4> slots{};
        unsigned count = 0;
        void record(unsigned slot) {
            if (count < times.size()) { times[count] = clock->now; slots[count] = slot; }
            ++count;
        }
        static void first(void* context) {
            auto& c = *static_cast<Chain*>(context); c.record(0);
            c.clock->cancel(3);
            c.clock->schedule(1, 20, second, &c);
            c.clock->schedule(2, 20, third, &c);
        }
        static void second(void* context) {
            auto& c = *static_cast<Chain*>(context); c.record(1);
            if (c.clock->now == 20) c.clock->schedule(1, 28, second, &c);
        }
        static void third(void* context) { static_cast<Chain*>(context)->record(2); }
    } chain{&s.memory.clock};
    start(s, 0xc000);
    for (unsigned i = 0; i < 4; ++i) s.memory.poke(0xc000 + i, 0);
    s.memory.clock.schedule(0, 4, Chain::first, &chain);
    s.memory.clock.schedule(1, 12, Chain::second, &chain);
    s.memory.clock.schedule(2, 16, Chain::third, &chain);
    s.memory.clock.schedule(3, 30, Chain::third, &chain);
    const auto events = s.run_until(32);
    if (events.instructions == 4 && events.time == 32 && s.cpu.pc == 0xc004 &&
        chain.count == 4 && chain.times == std::array<Tick, 4>{4, 20, 20, 28} &&
        chain.slots == std::array<unsigned, 4>{0, 1, 2, 1} && s.memory.clock.next == kNever) mask |= 8;
    return mask;
}
uint32_t linked_lifetime(System& s) {
    start(s, 0xc000);
    // Two branch destinations share their source. Keep taking the hot path
    // while enough other code runs to evict the cold destination, then return
    // through that old edge. Expected values follow JP/INC/LD M-cycle timings.
    // The other code fills WRAM from 0xC400 with 1792 blocks and runs most
    // of them again through echo RAM, as other blocks: 3456 in all, more
    // than the 2048 slots of the code pool, see Sm83Jit::initialize().
    constexpr uint8_t source[]{0xca,0x10,0xc0,0xc3,0x20,0xc0};
    constexpr uint8_t cold[]{0x04,0xc3,0x00,0xc0};
    constexpr uint8_t hot[]{0xc3,0x00,0xc0};
    for (unsigned i = 0; i < sizeof(source); ++i) s.memory.poke(0xc000 + i, source[i]);
    for (unsigned i = 0; i < sizeof(cold); ++i) s.memory.poke(0xc010 + i, cold[i]);
    for (unsigned i = 0; i < sizeof(hot); ++i) s.memory.poke(0xc020 + i, hot[i]);
    const auto run = [&](uint16_t pc, Tick ticks, unsigned count) {
        s.cpu.pc = pc;
        const auto target = s.memory.clock.now + ticks;
        const auto r = s.run_until(target);
        return r.end == RunEnd::target && r.time == target && r.instructions == count && s.cpu.pc == pc;
    };
    bool good = true;
    for (unsigned i = 0; i < 2; ++i) { s.cpu.f = 0x80; good &= run(0xc000, 72, 3); }
    uint32_t mask = good && s.cpu.b == 2 ? 1 : 0;
    const auto evicted = s.jit() ? s.jit()->evictions() : 0;
    for (unsigned i = 0; i < (0xe000 - 0xc400) / 4; ++i) {
        s.cpu.f = 0;
        good &= run(0xc000, 88, 3);
        const uint16_t pc = 0xc400 + i * 4;
        s.memory.poke(pc, 0x3e); s.memory.poke(pc + 1, static_cast<uint8_t>(i));
        s.memory.poke(pc + 2, 0x18); s.memory.poke(pc + 3, 0xfc);
        good &= run(pc, 40, 2) && s.cpu.a == static_cast<uint8_t>(i);
        if (pc < 0xde00) good &= run(static_cast<uint16_t>(pc + 0x2000), 40, 2) && s.cpu.a == static_cast<uint8_t>(i);
    }
    s.cpu.f = 0x80;
    if (good && run(0xc000, 72, 3) && s.cpu.b == 3 && !s.cpu.c &&
        (!s.jit() || s.jit()->evictions() > evicted)) mask |= 2;
    // Refetch at a linked destination must retire that destination, rather
    // than the first block entered by the C++ caller.
    s.memory.poke(0xc010, 0x0c); s.cpu.f = 0x80;
    if (run(0xc000, 72, 3) && s.cpu.b == 3 && s.cpu.c == 1) mask |= 4;
    s.set_trace(nullptr);  // Clears translations without resetting CPU/memory.
    s.cpu.f = 0x80;
    if (run(0xc000, 72, 3) && s.cpu.b == 3 && s.cpu.c == 2) mask |= 8;
    return mask;
}
}
bool mapped_blocks(std::array<uint32_t, 18>& results) {
    auto rom = std::unique_ptr<std::array<uint8_t, 32768>>(new (std::nothrow) std::array<uint8_t, 32768>{});
    auto system = std::unique_ptr<System>(new (std::nothrow) System);
    if (!rom || !system) return false;
    (*rom)[0x100] = 0x7e; (*rom)[0x101] = 0x3c;
    (*rom)[0x180] = 0xcb; (*rom)[0x181] = 0;
    constexpr uint8_t lazy[]{0x3e,0xff,0xc6,0x01,0x47,0x0e,0x01,0x89,0x40};
    std::copy(std::begin(lazy), std::end(lazy), rom->begin() + 0x110);
    if (!system->load_rom(*rom, true)) return false;
    for (unsigned native = 0; native < 2; ++native) {
        if (!system->select_backend(native ? Backend::jit : Backend::interpreter)) return false;
        results[native] = event_access(*system);
        results[2 + native] = wram_rewrite(*system);
        results[4 + native] = hram_self_modify(*system);
        results[6 + native] = lazy_trace(*system);
        results[8 + native] = fetched_opcode(*system) | (block_boundaries(*system) << 3);
        results[10 + native] = writable_refetch(*system);
        results[12 + native] = writable_cb(*system);
        results[14 + native] = batched_execution(*system);
        results[16 + native] = linked_lifetime(*system);
    }
    return true;
}
}  // namespace gbx::test
