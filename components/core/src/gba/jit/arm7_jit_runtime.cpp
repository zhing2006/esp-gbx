// What runs with generated code: Arm7Jit's dispatch loop and the runtime
// helpers that blocks call through the gateway's stubs. Those that games
// will run go to internal RAM with the execution core (components/core/
// linker.lf), apart from the compiler; the flat test bus stays in flash.

#include "arm7_jit_runtime.h"

#include "../executor.h"
#include "../mapped_bus.h"
#include "gbx/gba/system.h"

namespace gbx::gba::arm7_runtime {
namespace {

uint32_t host(const Frame* frame, R r) { return frame->host[static_cast<unsigned>(r)]; }

void switch_cpsr(Arm7State& cpu, uint32_t value) {
    arm7_switch_mode(cpu, cpu.cpsr, value);
    cpu.cpsr = value;
}

// A mapped helper's view of the run: Memory's countdown, R15, the opcodes
// open_bus() and the BIOS latch see and the prefetch buffer as Memory
// keeps them, so that MappedBus and the devices run as for the
// interpreter. done() hands the buffer back to native code where it can.
struct Mapped {
    Frame* frame;
    Memory& m;
    Arm7State& cpu;
    uint32_t head;  // The next code fetch.

    [[gnu::always_inline]] explicit Mapped(Frame* f) : frame(f), m(*f->memory), cpu(*f->cpu) {
        m.left = static_cast<int32_t>(host(f, kRemaining));
        const uint32_t pc = f->pc, r15 = pc & ~(kAfterPrefetch | kPcThumb);
        const bool after = pc & kAfterPrefetch;
        const uint32_t length = (after ? (pc & kPcThumb) != 0 : (cpu.cpsr & kFlagT) != 0) ? 2 : 4;
        head = after ? r15 + length : r15;
        cpu.r[15] = r15;
        const uint32_t last = head - length;
        m.lazy_pipeline = last | 1;
        if (last < kBiosBytes) m.latch_bios(last);
        if (f->prefetch_flags) m.return_prefetch(head, f->prefetch_base, f->prefetch_flags);
    }
    [[gnu::always_inline]] void done(uint32_t next) {
        lend_prefetch(m, next, frame->prefetch_base, frame->prefetch_flags);
        frame->host[static_cast<unsigned>(kRemaining)] = static_cast<uint32_t>(m.left);
    }
};

// Countdown and prefetch around a helper that may move the deadline
// without a bus cycle.
struct Countdown {
    Frame* frame;
    Memory& m;
    Tick deadline;
    explicit Countdown(Frame* f) : frame(f), m(*f->memory), deadline(f->memory->deadline) {
        m.left = static_cast<int32_t>(host(f, kRemaining));
    }
    ~Countdown() {
        frame->host[static_cast<unsigned>(kRemaining)] = static_cast<uint32_t>(m.left);
        frame->prefetch_base += static_cast<int32_t>(static_cast<int64_t>(deadline) - static_cast<int64_t>(m.deadline));
    }
};

}  // namespace

void lend_prefetch(Memory& memory, uint32_t next, int32_t& base, uint32_t& flags) {
    flags = memory.lend_prefetch(next, base);
}

void return_prefetch(Memory& memory, uint32_t head, int32_t base, uint32_t flags) {
    if (flags) memory.return_prefetch(head, base, flags);
}

#ifdef GBX_PROFILER
static void count_access(unsigned kind, uint32_t address) {
    auto& counts = arm7_access_counts[kind];
    ++counts[(address >> 24) & 15];
    if ((address >> 24) == 4 && (address & 0xffffff) < 0x210) ++counts[16 + ((address & 0x3ff) >> 1)];
}
#endif

uint32_t mapped_catch_fetch(Frame* frame) {
    GBX_CYCLE_SPAN(9);
    Mapped s(frame);
    s.m.catch_up_fetch();
    s.done(s.head);
    return 0;
}

uint32_t mapped_catch(Frame* frame) {
    GBX_CYCLE_SPAN(9);
#ifdef GBX_PROFILER
    ++arm7_cycle_counts[18];
    if (frame->memory->irq_watched()) ++arm7_cycle_counts[23];
#endif
    Mapped s(frame);
    s.m.catch_up();
    s.done(s.head);
    return 0;
}

uint32_t mapped_idle(Frame* frame) {
    GBX_CYCLE_SPAN(10);
    Mapped s(frame);
    s.m.idle_slow();
    s.done(s.head);
    return 0;
}

uint32_t mapped_read(Frame* frame, uint32_t request, uint32_t address) {
    GBX_CYCLE_SPAN(3);
#ifdef GBX_PROFILER
    ++arm7_cycle_counts[17];
    count_access(1, address);
#endif
    Mapped s(frame);
    const uint32_t value = MappedBus{s.m}.access<false>(request, address);
    s.done(s.head);
    return value;
}

// A write to a line of translated code may retire the code the pipeline
// holds: its opcodes as they are before the write, see
// Arm7Jit::code_written().
uint32_t mapped_write(Frame* frame, uint32_t request, uint32_t address, uint32_t data) {
    GBX_CYCLE_SPAN(2);
#ifdef GBX_PROFILER
    ++arm7_cycle_counts[16];
    count_access(0, address);
#endif
    Mapped s(frame);
    auto& c = *frame->context;
    const uint32_t offset = address & 0xffffff;
    const unsigned region = address >> 24;
    const bool marked = region == 3   ? s.m.iwram_code[(offset & 0x7fff) >> Memory::kCodeLineBits] != 0
                        : region == 2 ? s.m.ewram_code[(offset & 0x3ffff) >> Memory::kCodeLineBits] != 0
                                      : false;
    if (marked && !c.snapped) {
        const bool thumb = s.cpu.cpsr & kFlagT;
        const uint32_t last = s.head - (thumb ? 2 : 4);
        c.snap = {s.m.peek(last - (thumb ? 2 : 4), thumb ? 1 : 2), s.m.peek(last, thumb ? 1 : 2)};
        c.snap_last = last;
        c.snapped = true;
    }
    MappedBus{s.m}.access<false>(request, address, data);
    if (marked && !c.smc) c.snapped = false;
    s.done(s.head);
    return 0;
}

uint32_t mapped_fetch(Frame* frame, uint32_t request, uint32_t address) {
    GBX_CYCLE_SPAN(8);
    Mapped s(frame);
    if (request & kStateSequential) request = (request & ~kStateSequential) | (s.cpu.sequential ? kSequential : 0);
    MappedBus{s.m}.access<false>(request, address);
    s.done(address + (((request >> 2) & 3) == 2 ? 4 : 2));
    return 0;
}

uint32_t mapped_refill(Frame* frame, uint32_t length, uint32_t target) {
    GBX_CYCLE_SPAN(8);
    Mapped s(frame);
    const MappedBus bus{s.m};
    const bool checked = length & kRefillChecked;
    length &= ~kRefillChecked;
    const uint32_t width = length == 2 ? kHalf : kWord;
    if (checked) bus.access<false>(kFetch | width | kRefill, target);
    else bus.access<true>(kFetch | width | kRefill, target);
    bus.access<true>(kFetch | width | kSequential, target + length);
    s.done(target + 2 * length);
    return 0;
}

uint32_t mapped_boundary(Frame* frame) {
    GBX_CYCLE_SPAN(4);
#ifdef GBX_PROFILER
    ++arm7_cycle_counts[19];
    if (frame->memory->irq_watched()) ++arm7_cycle_counts[24];
#endif
    Mapped s(frame);
    auto& m = s.m;
    uint32_t stop = m.stop;
    if (!stop) {
        m.catch_up_boundary();
        stop = m.stop;
        if (!stop && m.left <= 0) m.catch_up_fetch();
    }
    s.done(s.head);
    return stop;
}

uint32_t mapped_link_boundary(Frame* frame) {
    GBX_CYCLE_SPAN(4);
#ifdef GBX_PROFILER
    ++arm7_cycle_counts[19];
#endif
    Mapped s(frame);
    auto& m = s.m;
    if (!m.stop) m.catch_up_boundary();
    s.done(s.head);
    return m.stop;
}

// The flags a side exit leaves pending are computed as the compiler would
// (Compiler::materialize_nz() and the others in arm7_jit.cpp), from the
// host registers the stub saved.
uint32_t side_exit(Frame* frame, const uint32_t* descriptor) {
    auto& cpu = *frame->cpu;
    for (unsigned i = 0; i < kGuest.size(); ++i) cpu.r[i] = host(frame, kGuest[i]);
    const uint32_t bits = descriptor[1];
    uint32_t n = host(frame, kN) >> 31, z = host(frame, kZ) == 0, c = host(frame, kC) & 1, v = host(frame, kV) >> 31;
    const uint32_t arith = bits >> side_bits::arith;
    uint32_t result = 0, first = 0, second = 0;
    if (arith != kArithNone) {
        const uint32_t word = descriptor[2];
        const uint32_t* immediate = descriptor + 3;
        bool known[3];
        uint32_t values[3];
        for (unsigned i = 0; i < 3; ++i) {
            const uint32_t code = (word >> (6 * i)) & 63;
            known[i] = code != kOperandUnknown;
            values[i] = code == kOperandImmediate ? *immediate++ : known[i] ? frame->host[code] : 0;
        }
        result = values[0], first = values[1], second = values[2];
        if (arith == kArithAdd) {
            if (!known[0]) result = first + second;
            else if (!known[1]) first = result - second;
            else if (!known[2]) second = result - first;
        } else {
            if (!known[0]) result = first - second;
            else if (!known[1]) first = result + second;
            else if (!known[2]) second = first - result;
        }
    }
    const uint32_t nz_register = (bits >> side_bits::nz_register) & 31;
    switch ((bits >> side_bits::nz) & 3) {
    case kNzRegister: n = frame->host[nz_register] >> 31; z = frame->host[nz_register] == 0; break;
    case kNzArith: n = result >> 31; z = result == 0; break;
    default: break;
    }
    const uint32_t c_first = (bits >> side_bits::c_first) & 31, c_second = (bits >> side_bits::c_second) & 31;
    switch ((bits >> side_bits::c) & 7) {
    case kCArith: c = arith == kArithAdd ? result < first : first >= second; break;
    case kCBit: c = (frame->host[c_first] >> c_second) & 1; break;
    case kCZero: c = 0; break;
    case kCOne: c = 1; break;
    case kCMultiply: {
        const uint32_t multiplier = frame->host[c_second];
        c = gba::multiply_carry(frame->host[c_first], multiplier, 0, true, false, multiply_cycles(multiplier, true));
        break;
    }
    default: break;
    }
    if (bits & side_bits::v)
        v = (arith == kArithAdd ? (first ^ result) & (second ^ result) : (first ^ second) & (first ^ result)) >> 31;
    cpu.cpsr = (cpu.cpsr & 0x0fffffff) | n << 31 | z << 30 | c << 29 | v << 28;
    cpu.r[15] = descriptor[0];
    cpu.sequential = bits & side_bits::sequential_request ? (host(frame, R::a1) & kSequential) != 0
                                                          : (bits & side_bits::sequential) != 0;
    auto& context = *frame->context;
    context.remaining = static_cast<int32_t>(host(frame, kRemaining));
    context.instructions += bits & 0x7f;
    context.prefetch_base = frame->prefetch_base;
    context.prefetch_flags = frame->prefetch_flags;
    context.link = nullptr;
    return static_cast<uint32_t>(Arm7JitExit::budget);
}

// The mapped bus's checks after CPSR changed (MappedBus::psr_written() and
// irq_masked_late()): the run stops at the next boundary for an interrupt
// the line holds.
uint32_t write_cpsr(Frame* frame, uint32_t value, uint32_t mask) {
    auto& cpu = *frame->cpu;
    switch_cpsr(cpu, (cpu.cpsr & ~mask) | (value & mask) | 0x10);
    if (Memory* m = frame->memory; m && m->irq_line && !(cpu.cpsr & kFlagI)) {
        Countdown countdown(frame);
        m->request_stop();
    }
    return 0;
}

uint32_t restore_cpsr(Frame* frame, uint32_t test) {
    auto& cpu = *frame->cpu;
    const bool unmasked = !(cpu.cpsr & kFlagI);
    if (const unsigned bank = bank_of(cpu.cpsr)) switch_cpsr(cpu, cpu.spsr[bank - 1]);
    Memory* m = frame->memory;
    if (!m || !m->irq_line) return 0;
    const bool late = test && unmasked;
    if (late || !(cpu.cpsr & kFlagI)) {
        Countdown countdown(frame);
        if (late) m->irq_unmasked = true;
        m->request_stop();
    }
    return 0;
}

uint32_t exception(Frame* frame, uint32_t mode, uint32_t return_address) {
    auto& cpu = *frame->cpu;
    const uint32_t old = cpu.cpsr;
    switch_cpsr(cpu, (old & ~(kModeMask | kFlagT)) | mode | kFlagI);
    cpu.spsr[bank_of(mode) - 1] = old;
    cpu.r[14] = return_address;
    return 0;
}

uint32_t multiply_carry(Frame*, uint32_t multiplicand, uint32_t multiplier, uint32_t kind,
                        uint32_t accumulator_low, uint32_t accumulator_high) {
    const bool is_signed = kind & kMultiplySigned, is_long = kind & kMultiplyLong;
    const uint64_t accumulator =
        kind & kMultiplyAccumulate ? (uint64_t{accumulator_high} << 32 | accumulator_low) : 0;
    return gba::multiply_carry(multiplicand, multiplier, accumulator, is_signed, is_long,
                               multiply_cycles(multiplier, is_signed));
}

uint32_t flat_access(Frame* frame, uint32_t request, uint32_t address, uint32_t data) {
    return frame->context->bus->access(request, address, data);
}

uint32_t flat_fetch(Frame* frame, uint32_t request, uint32_t address) {
    auto& cpu = *frame->cpu;
    if (request & kStateSequential) request = (request & ~kStateSequential) | (cpu.sequential ? kSequential : 0);
    const uint32_t value = frame->context->bus->access(request, address);
    cpu.pipeline[0] = cpu.pipeline[1];
    cpu.pipeline[1] = value;
    return value;
}

}  // namespace gbx::gba::arm7_runtime

namespace gbx::gba {

using namespace arm7_runtime;

namespace {

// Counts the bus cycles of an instruction that the interpreter runs instead.
struct CountingBus {
    const Bus* bus;
    uint32_t cycles = 0;
    static uint32_t cycle(void* context, uint32_t request, uint32_t address, uint32_t data) {
        auto& self = *static_cast<CountingBus*>(context);
        ++self.cycles;
        return self.bus->access(request, address, data);
    }
};

}  // namespace

// --- Mapped execution ----------------------------------------------------------

namespace {

// Code the JIT translates: the BIOS, EWRAM, IWRAM and the ROM, with the
// instruction and its prefetch in the same region.
bool translatable(uint32_t r15, uint32_t cpsr) {
    const uint32_t first = r15 - (cpsr & kFlagT ? 4 : 8);
    const unsigned region = r15 >> 24;
    if (region != first >> 24) return false;
    return region == 0 ? r15 < kBiosBytes : region == 2 || region == 3 || (region >= 8 && region < 14);
}

// Whether the pipeline holds other opcodes than memory now does, after a
// write to code it had fetched: blocks are translated from memory.
bool stale(const Arm7State& cpu, Memory& m) {
    const bool thumb = cpu.cpsr & kFlagT;
    const uint32_t length = thumb ? 2 : 4, r15 = cpu.r[15];
    return cpu.pipeline[0] != m.peek(r15 - 2 * length, thumb ? 1 : 2) ||
           cpu.pipeline[1] != m.peek(r15 - length, thumb ? 1 : 2);
}

}  // namespace

unsigned Arm7Jit::interpret(Arm7State& state, unsigned count) {
    Memory& m = *memory_;
    unsigned steps = 0;
    if (!count) {
        do arm7_step_mapped(state, m), ++steps;
        while (m.left > 0 && !translatable(state.r[15], state.cpsr));
        return steps;
    }
    for (; steps < count; ++steps) {
        if (steps && m.left <= 0) break;
        arm7_step_mapped(state, m);
    }
    return steps;
}

// A CPU or DMA write to a line of RAM code: the blocks whose code it
// changed die. The run stops at the next boundary, and the next exit
// retires them before anything else runs, see run().
void Arm7Jit::code_written(void* self, uint32_t address) {
    auto& jit = *static_cast<Arm7Jit*>(self);
    const unsigned region = address >> 24;
    const uint32_t word = region == 3 ? 0x03000000 | (address & 0x7ffc) : 0x02000000 | (address & 0x3fffc);
    const unsigned bit = code_word(word);
    uint32_t& bits = jit.code_words_[bit >> 5];
    if (!(bits & (1u << (bit & 31)))) return;
    bool hit = false, covered = false;
    jit.each_ram(word, [&](unsigned i) {
        auto& ram = jit.ram_blocks_[i];
        if (ram.begin < word + 4 && word < ram.end) {
            if (!ram.dead) {
                ram.dead = hit = true;
                if (jit.dead_count_ < kDeadBlocks) jit.dead_blocks_[jit.dead_count_] = {ram.block, ram.begin};
                ++jit.dead_count_;
            }
            covered = true;
        }
        return false;
    });
    if (!covered) bits &= ~(1u << (bit & 31));
    if (!hit) return;
    jit.dead_ = true;
    Memory& m = *jit.memory_;
    if (auto* c = jit.active_; c && m.lazy_pipeline) {
        // In native code: the pipeline as it was, unless the write was
        // the CPU's, which mapped_write() saw first.
        c->smc = true;
        if (!c->snapped) {
            const bool thumb = m.cpu->cpsr & kFlagT;
            const uint32_t last = m.lazy_pipeline & ~1u;
            c->snap = {m.peek(last - (thumb ? 2 : 4), thumb ? 1 : 2), m.peek(last, thumb ? 1 : 2)};
            c->snap_last = last;
            c->snapped = true;
        }
    }
    m.request_stop();
}

void Arm7Jit::retire_dead() {
    dead_ = false;
    if (dead_count_ <= kDeadBlocks) {
        for (unsigned d = 0; d < dead_count_; ++d) {
            const auto [block, begin] = dead_blocks_[d];
            unsigned found = kRamBlocks;
            each_ram(begin, [&](unsigned i) {
                if (ram_blocks_[i].block != block || !ram_blocks_[i].dead) return false;
                found = i;
                return true;
            });
            if (found != kRamBlocks && retire_ram(found)) ++invalidations_;
        }
        dead_count_ = 0;
        return;
    }
    dead_count_ = 0;
    for (unsigned i = 0; i < ram_count_;) {
        if (!ram_blocks_[i].dead) {
            ++i;
            continue;
        }
        if (retire_ram(i)) ++invalidations_;
    }
}

// arm7_run_mapped() with blocks: the same boundaries, in the same order.
// Native code runs on while cycles are left and through boundaries where
// events come due (mapped_boundary()); C++ takes the stops, interrupts and
// HALT, the blocks' misses and the code the JIT does not translate.
void Arm7Jit::run(Arm7State& cpu, Memory& m) {
    GBX_CYCLE_SPAN(0);
    Context context{&cpu, nullptr, 0, 0};
    context.memory = &m;
    context.dispatch = &tables_->dispatch[0].r15;
    context.tags = tags_;
    active_ = &context;
#ifdef GBX_PROFILER
    static bool logged = false;
    if (!logged) {
        logged = true;
        platform::log(platform::LogLevel::warn, "arm7-jit", "addresses: context %p tables %p cpu %p memory %p iwram %p jit %p mid %p",
                      static_cast<void*>(&context), static_cast<void*>(tables_.get()), static_cast<void*>(&cpu),
                      static_cast<void*>(&m), static_cast<void*>(m.iwram.data()), static_cast<void*>(this),
                      static_cast<void*>(mid_.get()));
    }
#endif
    // A static exit to link to the next block, and the retirements before
    // its lookup, which may have retired the exit's own block.
    uint32_t* link = nullptr;
    uint32_t retirements = 0;
    // A run enters at an inner boundary of a block only where it did not
    // leave through a static exit.
    bool resume = true;
    // Instructions run on the interpreter and stops taken, for the observer.
    uint32_t steps = 0, stops = 0;
    for (;;) {
#ifdef GBX_PROFILER  // The run loop's turns, for stat.
        ++context.counts[6];
#endif
        if (dead_) retire_dead();
        // A stop keeps the countdown out (Memory::request_stop()).
        if (m.left <= 0 && !m.stop) m.catch_up_boundary();
        if (observer_) [[unlikely]]
            observer_(observer_context_, context.instructions + steps, stops, false);
        if (m.stop) {
            m.stop = false;
            ++stops;
#ifdef GBX_PROFILER  // Stops, and why.
            ++context.counts[9];
            if (m.irq_line && (!(cpu.cpsr & kFlagI) || m.irq_unmasked)) ++arm7_find_counts[15];
            else if (m.now() >= m.target) ++arm7_find_counts[13];
            else {
                ++arm7_find_counts[m.halted ? 14 : 12];
                auto& site = arm7_exit_sites[(cpu.r[15] >> 1) % arm7_exit_sites.size()];
                if (site.r15 != cpu.r[15])
                    site = {cpu.r[15], 0,
                            (cpu.cpsr & 0xff) | (m.irq_line ? 0x100u : 0) | (m.halted ? 0x200u : 0) | (m.ime() ? 0x400u : 0) |
                                (m.wake_pending() ? 0x800u : 0) | uint32_t{m.ie()} << 16};
                ++site.count;
            }
#endif
            if (m.halted) {
                GBX_CYCLE_SPAN(5);
                m.run_halt();
                if (m.halted) break;  // The target came first.
            }
            if (m.irq_line && (!(cpu.cpsr & kFlagI) || m.irq_unmasked)) {
                GBX_CYCLE_SPAN(6);
                arm7_interrupt_mapped(cpu, m);
            }
            m.irq_unmasked = false;
            if (m.now() >= m.target) {
                m.stop = true;
                break;
            }
            // The interrupt's entry took cycles since the clock last
            // caught up.
            m.catch_up_boundary();
            link = nullptr;
            resume = true;
            continue;
        }
        if (m.waits_generation != waits_generation_) update_tags();
        const uint32_t r15 = cpu.r[15];
        if (((r15 >> 24) | 1) == 3 && stale(cpu, m)) {
            // Opcodes the pipeline fetched before a write to them run as
            // they were fetched, on the interpreter.
            arm7_step_mapped(cpu, m);
            ++steps;
            link = nullptr;
            resume = true;
            continue;
        }
        if (!translatable(r15, cpu.cpsr)) {
            ++fallbacks_[static_cast<unsigned>(Arm7JitFallback::region)];
            steps += interpret(cpu, 0);
            link = nullptr;
            resume = true;
            continue;
        }
        const jit::BlockKey key = Arm7Jit::key(r15, keyhi(r15, cpu.cpsr));
        const uint32_t before = retired();
        uintptr_t target = 0;
        bool start = true;
        if (const auto entry = cache_.find(key)) {
            target = reinterpret_cast<uintptr_t>(entry) + kHeaderWords * 4;
        } else {
#ifdef GBX_PROFILER  // Lookups outside the code cache.
            ++context.counts[7];
#endif
#ifdef GBX_PROFILER
            const uint32_t find_start = CycleSpan::now();
#endif
            const Found found = find_block(cpu, key, resume);
#ifdef GBX_PROFILER
            arm7_cycle_counts[7] += CycleSpan::now() - find_start;
#endif
            if (!found.target) {
                if (found.failure != Arm7JitFallback::none) ++fallbacks_[static_cast<unsigned>(found.failure)];
                steps += interpret(cpu, found.interpret ? found.interpret : 1);
                link = nullptr;
                resume = true;
                continue;
            }
            target = found.target;
            start = found.start;
            context.instructions -= found.skipped;
            if (found.dispatchable && (!observer_ || observe_linked_)) dispatch_boundary(r15, key, target, found.skipped);
        }
        if (start && (!observer_ || observe_linked_)) {
            // The exit that asked for this block is still there only if no
            // block was retired since.
            if (link && retirements == before && retired() == before)
                this->link(link, target);
            auto& entry = tables_->dispatch[((r15 >> 1) + (key >> 32)) & (kDispatch - 1)];
            entry = {r15, static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(target)};
        }
#ifdef GBX_GBA_RELAXED_TIMING
        // The block's first fetch catches up here, so that its entry check
        // (Compiler::m_entry()), which takes a boundary for a linked exit,
        // finds cycles left. As in mapped_catch_fetch(), a DMA there that
        // rewrites the opcodes the pipeline holds leaves them as they were
        // fetched (code_written()).
        if (m.left <= 0 && !m.stop) {
            m.lazy_pipeline = (r15 - (cpu.cpsr & kFlagT ? 2 : 4)) | 1;
            m.catch_up_fetch();
        }
#endif
        context.remaining = m.left;
        context.link = nullptr;
        context.spent = 0;
        lend_prefetch(m, r15, context.prefetch_base, context.prefetch_flags);
#ifdef GBX_PROFILER  // Entries into native code, and why it left.
        ++context.counts[8];
#endif
#ifdef GBX_PROFILER
        {
            GBX_CYCLE_SPAN(1);
            enter(&context, target);
        }
#else
        enter(&context, target);
#endif
#ifdef GBX_PROFILER
        if (context.spent) {
            ++context.counts[12];
            if (cache_.find(Arm7Jit::key(cpu.r[15], keyhi(cpu.r[15], cpu.cpsr)))) ++context.counts[15];
        }
        else if (context.link) ++context.counts[10];
        else if (m.stop) ++context.counts[13];
        else if (context.remaining <= 0) ++context.counts[11];
        else {
            ++context.counts[14];
        }
#endif
        m.left = context.remaining;
        return_prefetch(m, cpu.r[15], context.prefetch_base, context.prefetch_flags);
        // The pipeline and the BIOS latch as the interpreter keeps them, with
        // the opcodes fetched before a write to code; those the interpreter
        // then runs.
        const bool thumb = cpu.cpsr & kFlagT;
        const uint32_t length = thumb ? 2 : 4, next = cpu.r[15];
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t address = next - (2 - i) * length;
            cpu.pipeline[i] = context.snapped && address == context.snap_last            ? context.snap[1]
                              : context.snapped && address == context.snap_last - length ? context.snap[0]
                                                                                          : m.peek(address, thumb ? 1 : 2);
        }
        m.lazy_pipeline = 0;
        if (next - length < kBiosBytes) m.latch_bios(next - length);
        context.smc = context.snapped = false;
        link = context.link && !*context.link ? context.link : nullptr;
        retirements = retired();
        // A block in the second level whose entries ran out comes back to
        // find_block() as a block start, not as a boundary of another block.
        resume = !link && !context.spent;
    }
    active_ = nullptr;
    interpreted_ += steps;
#ifdef GBX_PROFILER
    for (size_t i = 0; i < context.counts.size(); ++i) arm7_gateway_counts[i] += context.counts[i];
    arm7_cycle_counts[27] += context.instructions + steps;
#endif
    if (observer_) observer_(observer_context_, context.instructions + steps, stops, true);
}

uint32_t Arm7Jit::enter(void* context, uintptr_t entry) const {
    using Gateway = uint32_t (*)(void*, uintptr_t);
    return reinterpret_cast<Gateway>(stubs_.enter)(context, entry);
}

Arm7JitResult Arm7Jit::run(Arm7State& state, const Bus& bus, Arm7CodeSource code, int32_t& budget,
                           unsigned limit) {
    Arm7JitResult result;
    limit = limit < 1 ? 1 : limit > kMaxBlock ? kMaxBlock : limit;
    arm7_runtime::Context context{&state, &bus, budget, 0};
    while (context.remaining > 0) {
        // The entry R15, T and the mode name a block: its code depends on
        // nothing else. Milestone 2 adds the code's memory region and its
        // wait states.
        const jit::BlockKey key = state.r[15] | uint64_t{(state.cpsr & kFlagT) != 0} << 32 |
                                  uint64_t{state.cpsr & kModeMask} << 33 | uint64_t{limit - 1} << 38;
        jit::BlockEntry entry = available() ? cache_.find(key) : nullptr;
        Arm7JitFallback failure = Arm7JitFallback::unavailable;
        if (!entry && available()) {
            failure = Arm7JitFallback::none;
            if (const size_t words = compile(state, code, limit, failure)) {
                const auto installed = cache_.install_replacing(key, {output_.data(), words},
                                                                {relocations_.data(), relocation_count_});
                entry = installed.entry;
                if (installed.status == jit::InstallStatus::ok) ++compiled_blocks_;
                else if (installed.status == jit::InstallStatus::sync_failed) failure = Arm7JitFallback::publication;
                else failure = Arm7JitFallback::capacity;
            }
        }
        if (!entry) {
            ++fallbacks_[static_cast<unsigned>(failure)];
            result.fallback = failure;
            result.exit = Arm7JitExit::fallback;
            CountingBus counter{&bus};
            arm7_step(state, {&counter, CountingBus::cycle});
            context.remaining -= static_cast<int32_t>(counter.cycles);
            ++context.instructions;
            continue;
        }
        result.exit = static_cast<Arm7JitExit>(enter(&context, reinterpret_cast<uintptr_t>(entry)));
    }
    budget = context.remaining;
    result.instructions = context.instructions;
    return result;
}

}  // namespace gbx::gba
