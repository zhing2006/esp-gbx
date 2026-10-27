#include "cpu_peripherals.h"

#include <algorithm>
#include <memory>
#include <new>
#include "gbx/gb/system.h"

namespace gbx::test {
namespace {
using namespace gb;
void start(System& s, std::initializer_list<uint8_t> code = {}) {
    s.reset(); s.memory.clock.reset(); s.cpu = {}; s.cpu.pc = 0xc000; s.cpu.sp = 0xfffe;
    unsigned at = 0xc000;
    for (auto value : code) s.memory.poke(at++, value);
    s.memory.poke(0xff0f, 0);
}
bool instructions(System& s, unsigned count) {
    while (count) {
        const auto r = s.step_block(kTicksPerSecond, count);
        if (!r.instructions || r.instructions > count || r.fallback != JitFallback::none) return false;
        count -= r.instructions;
    }
    return true;
}
struct Event {
    Memory* memory;
    uint16_t address;
    uint8_t value;
    static void fire(void* context) {
        auto& e = *static_cast<Event*>(context);
        e.memory->poke(e.address, e.value);
    }
};
uint32_t interrupt_boundaries(System& s) {
    uint32_t mask = 0;
    start(s, {0x7e, 0x3c}); s.cpu.ime = true; s.cpu.set_hl(0xc100);
    s.memory.poke(0xc100, 0x55); s.memory.poke(0xffff, 4);
    Event event{&s.memory, 0xff0f, 4}; s.memory.clock.schedule(3, 10, Event::fire, &event);
    auto r = s.step_block(1000);
    if (r.instructions == 1 && s.cpu.a == 0x55 && s.cpu.pc == 0xc001 && s.memory.clock.now == 16) mask |= 1;
    r = s.step_block(1000);
    if (!r.instructions && r.mcycles == 5 && s.cpu.pc == 0x50 && !s.cpu.ime &&
        s.cpu.sp == 0xfffc && s.memory.peek(0xfffc) == 1 && s.memory.peek(0xfffd) == 0xc0 &&
        !(s.memory.peek(0xff0f) & 4) && s.memory.clock.now == 56) mask |= 2;
    for (unsigned which = 0; which < 2; ++which) {
        // LD A,4; LD (IE),A or LD (IF),A; INC A must wait for service.
        start(s, {0x3e, 4, 0xea, static_cast<uint8_t>(which ? 0x0f : 0xff), 0xff, 0x3c});
        s.cpu.ime = true;
        s.memory.poke(0xffff, which ? 4 : 0); s.memory.poke(0xff0f, which ? 0 : 4);
        r = s.step_block(1000);
        const bool stopped = r.instructions == 2 && s.cpu.pc == 0xc005 && s.cpu.a == 4;
        r = s.step_block(1000);
        if (stopped && !r.instructions && s.cpu.pc == 0x50 && s.cpu.a == 4) mask |= 4u << which;
    }
    start(s, {0xfb, 0, 0x3c}); s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    r = s.step_block(1000);
    const bool delayed = r.instructions == 2 && s.cpu.pc == 0xc002 && s.cpu.ime;
    r = s.step_block(1000);
    if (delayed && !r.instructions && s.cpu.pc == 0x50 && !s.cpu.a) mask |= 16;
    start(s, {0xd9}); s.cpu.sp = 0xc100;
    s.memory.poke(0xc100, 0x10); s.memory.poke(0xc101, 0xc0);
    s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    r = s.step_block(1000, 1);
    const bool reti = r.instructions == 1 && s.cpu.ime && s.cpu.pc == 0xc010;
    r = s.step_block(1000);
    if (reti && !r.instructions && s.cpu.pc == 0x50 && r.fallback == JitFallback::none) mask |= 32;
    start(s); s.cpu.ime = true; s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    std::array<TraceRecord, 4> records{}; TraceBuffer trace; trace.reset(records);
    s.set_trace(&trace); r = s.step_block(1000);
    TraceRecord high{}, low{}, extra{};
    if (!r.instructions && trace.pop(high) && trace.pop(low) && !trace.pop(extra) && !trace.dropped() &&
        high.time == 22 && high.pc == 0xc000 && high.address == 0xfffd && high.data == 0xc0 && high.operation == 1 &&
        low.time == 30 && low.pc == 0xc000 && low.address == 0xfffc && low.data == 0 && low.operation == 1) mask |= 64;
    s.set_trace(nullptr);
    return mask;
}
uint32_t halt_paths(System& s) {
    uint32_t mask = 0;
    start(s, {0x76, 0x3c}); s.memory.poke(0xffff, 4);
    Event event{&s.memory, 0xff0f, 4}; s.memory.clock.schedule(3, 19, Event::fire, &event);
    const auto halted = s.step_block(1000), wait = s.step_block(1000);
    if (halted.instructions == 1 && !wait.instructions && s.memory.clock.now == 24 &&
        s.cpu.mode == CpuMode::running && !s.cpu.ime && instructions(s, 1) && s.cpu.a == 1) mask |= 1;
    start(s, {0x76, 0x3e, 0x12}); s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    if (instructions(s, 2) && s.cpu.a == 0x3e && s.cpu.pc == 0xc002 && !s.cpu.halt_bug) mask |= 2;
    start(s, {0x76, 0xcb}); s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    if (instructions(s, 2) && s.cpu.e == 2 && s.cpu.pc == 0xc002) mask |= 4; // CB CB = SET 1,E.
    start(s, {0x76}); s.cpu.ei_pending = true; s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, 4);
    s.step_block(1000); const auto irq = s.step_block(1000);
    if (!irq.instructions && s.cpu.pc == 0x50 && !s.cpu.halt_bug &&
        s.memory.peek(0xfffc) == 0 && s.memory.peek(0xfffd) == 0xc0) mask |= 8;
    // A changed bugged fetch must not be duplicated by native continuation.
    start(s); s.cpu.halt_bug = true;
    event = {&s.memory, 0xc000, 0x3e}; s.memory.clock.schedule(3, 4, Event::fire, &event);
    if (instructions(s, 1) && s.cpu.a == 0x3e && s.cpu.pc == 0xc001 && s.memory.clock.now == 16) mask |= 16;
    start(s, {0xd3}); instructions(s, 1);
    s.cpu.ime = true; s.memory.poke(0xffff, 31); s.memory.poke(0xff0f, 31);
    const auto locked = s.run_until(1000);
    if (locked.end == RunEnd::suspended && s.cpu.pc == 0xc001 && s.cpu.mode == CpuMode::locked) mask |= 32;
    return mask;
}
uint32_t stop_joypad(System& s) {
    uint32_t mask = 0;
    for (unsigned held = 0; held < 2; ++held) for (unsigned pending = 0; pending < 2; ++pending) {
        start(s, {0x10}); s.memory.poke(0xff00, 0x20);
        s.set_buttons(held); s.memory.poke(0xffff, 4); s.memory.poke(0xff0f, pending ? 4 : 0);
        const auto div = s.memory.divider();
        const auto r = s.step_block(1000, 1);
        const auto mode = held ? (pending ? CpuMode::running : CpuMode::halted) : CpuMode::stopped;
        if (r.instructions == 1 && r.fallback == JitFallback::none && s.cpu.pc == 0xc002 - pending &&
            s.cpu.mode == mode && s.memory.divider() == (held ? static_cast<uint16_t>(div + 4) : 0))
            mask |= 1u << (held * 2 + pending);
    }
    start(s, {0x10, 0, 0x3c}); s.memory.poke(0xff00, 0x30);
    const auto stop = s.run_until(1000);
    s.set_buttons(1);
    const auto still = s.run_until(1000);
    const bool frozen = stop.end == RunEnd::suspended && still.end == RunEnd::suspended &&
        still.time == stop.time && !s.memory.divider() && !(s.memory.peek(0xff0f) & 16);
    s.memory.poke(0xff00, 0x20); // Selecting the held row creates a falling edge.
    s.step_block(1000);
    if (frozen && instructions(s, 1) && s.cpu.a == 1 && s.memory.divider() == 4 &&
        (s.memory.peek(0xff0f) & 16)) mask |= 16;
    s.memory.poke(0xff0f, 0); s.memory.poke(0xff00, 0); s.set_buttons(0x11);
    const bool overlap = !(s.memory.peek(0xff0f) & 16) && (s.memory.peek(0xff00) & 15) == 14;
    s.set_buttons(0x31);
    if (overlap && (s.memory.peek(0xff0f) & 16) && (s.memory.peek(0xff00) & 15) == 12) mask |= 32;
    return mask;
}
uint32_t timer_reload_writes(System& s) {
    uint32_t mask = 0;
    // Pan Docs, Timer obscure behaviour: TIMA writes during reload are
    // ignored; TMA writes feed TIMA only while the load window is active.
    // Exercise adjacent bus writes in real instructions, not direct pokes.
    for (unsigned traced = 0; traced < 2; ++traced) for (unsigned test = 0; test < 4; ++test) {
        if (test == 0) {
            start(s, {0x08, 0x04, 0xff}); // LD (DIV),SP: DIV then TIMA.
            s.cpu.sp = 0x7700;
        } else if (test == 1) {
            start(s, {0xc5}); // PUSH BC: TAC then TMA.
            s.cpu.sp = 0xff08; s.cpu.set_bc(0x0077);
        } else {
            start(s, {0xe0, 0x04, 0x77}); // LDH (DIV),A; LD (HL),A.
            s.cpu.a = 0x77; s.cpu.set_hl(test == 2 ? 0xff05 : 0xff06);
        }
        s.memory.poke(0xff04, 0);
        s.memory.clock.advance(1024); // DIV bit 9 high, far from its next falling edge.
        s.memory.poke(0xff05, 0xff); s.memory.poke(0xff06, 0x42); s.memory.poke(0xff07, 4);
        std::array<TraceRecord, 8> records{};
        TraceBuffer trace; trace.reset(records);
        s.set_trace(traced ? &trace : nullptr);
        bool good = instructions(s, test < 2 ? 1 : 2) &&
            s.memory.peek(0xff05) == (test == 1 || test == 2 ? 0x77 : 0x42) &&
            s.memory.peek(0xff06) == (test == 1 || test == 3 ? 0x77 : 0x42) &&
            (s.memory.peek(0xff0f) & 4);
        if (traced) {
            TraceRecord record{};
            Tick first_write = 0;
            unsigned writes = 0;
            while (trace.pop(record)) {
                if (record.operation != 1) continue;
                if (writes == 0) {
                    first_write = record.time;
                    good &= record.address == (test == 1 ? 0xff07 : 0xff04) &&
                        record.data == (test < 2 ? 0 : 0x77);
                } else if (writes == 1) {
                    good &= record.address == (test == 0 || test == 2 ? 0xff05 : 0xff06) &&
                        record.data == 0x77 && record.time - first_write == (test < 2 ? 8 : 16);
                }
                ++writes;
            }
            good &= writes == 2 && !trace.dropped();
        }
        s.set_trace(nullptr);
        if (good) mask |= 1u << (traced * 4 + test);
    }
    return mask;
}
uint32_t serial_transfer(System& s) {
    uint32_t mask = 0;
    start(s); s.cpu.pc = 0x200; s.memory.poke(0xff04, 0);
    s.memory.poke(0xff01, 0xa5); s.memory.poke(0xff02, 0x81);
    s.run_until(1024);
    if (s.memory.peek(0xff01) == 0x4b && (s.memory.peek(0xff02) & 0x80) && s.serial().empty()) mask |= 1;
    s.memory.poke(0xff01, 0x3c); // Remaining outgoing bits use the new SB value.
    s.run_until(8192);
    // SB was overwritten after the first received bit; only seven ones follow.
    if (s.memory.peek(0xff01) == 0x7f && !(s.memory.peek(0xff02) & 0x80) &&
        (s.memory.peek(0xff0f) & 8) && s.serial().size() == 1 && s.serial()[0] == 0x9e) mask |= 2;
    s.memory.poke(0xff0f, 0); s.memory.poke(0xff02, 0x81); s.run_until(9216);
    s.memory.poke(0xff02, 0); s.run_until(16384);
    if (s.serial().size() == 1 && !(s.memory.peek(0xff0f) & 8)) mask |= 4;
    // Starting between DIV edges must not shift their phase; HALT keeps them running.
    s.run_until(16640); s.memory.poke(0xff01, 0x55); s.memory.poke(0xff02, 0x81);
    s.cpu.mode = CpuMode::halted; s.memory.poke(0xffff, 8);
    s.run_until(24576);
    if (s.serial().size() == 2 && s.serial()[1] == 0x55 && s.cpu.mode == CpuMode::running &&
        !s.serial_truncated()) mask |= 8;
    return mask;
}
}
bool cpu_peripherals(std::array<uint32_t, 10>& results) {
    auto rom = std::unique_ptr<std::array<uint8_t, 32768>>(new (std::nothrow) std::array<uint8_t, 32768>{});
    auto s = std::unique_ptr<System>(new (std::nothrow) System);
    if (!rom || !s || !s->load_rom(*rom)) return false;
    for (unsigned native = 0; native < 2; ++native) {
        if (!s->select_backend(native ? Backend::jit : Backend::interpreter)) return false;
        results[native] = interrupt_boundaries(*s);
        results[2 + native] = halt_paths(*s);
        results[4 + native] = stop_joypad(*s);
        results[6 + native] = serial_transfer(*s);
        results[8 + native] = timer_reload_writes(*s);
        if (native && std::any_of(s->jit()->fallbacks().begin(), s->jit()->fallbacks().end(),
                                  [](uint64_t count) { return count != 0; })) return false;
    }
    return true;
}
}  // namespace gbx::test
