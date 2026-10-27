#include "sm83_blocks.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <new>

#include "gbx/gb/jit/sm83_jit.h"

namespace gbx::test {
namespace {
using namespace gb;

JitResult execute(bool native, Sm83Jit& jit, Sm83State& state, const Bus& bus, unsigned limit) {
    if (native) return jit.run_block(state, bus, limit);
    JitResult result;
    while (result.instructions < limit && state.mode == CpuMode::running) {
        result.mcycles += sm83_step(state, bus).mcycles;
        ++result.instructions;
    }
    return result;
}

uint32_t loop(bool native, Sm83Jit& jit, std::span<uint8_t, 65536> memory) {
    std::fill(memory.begin(), memory.end(), 0);
    // LD SP,d000; LD A,0; LD B,3; ADD A,B; DEC B; JR NZ,loop;
    // CALL 0120; HALT. Subroutine: ADD A,10; RET.
    constexpr uint8_t program[]{0x31,0x00,0xd0,0x3e,0x00,0x06,0x03,0x80,0x05,
                                0x20,0xfc,0xcd,0x20,0x01,0x76};
    std::copy(std::begin(program), std::end(program), memory.begin() + 0x100);
    memory[0x120] = 0xc6; memory[0x121] = 0x10; memory[0x122] = 0xc9;
    jit.clear();
    uint32_t masks[2]{};
    for (unsigned pass = 0; pass < 2; ++pass) {
        Sm83State state; state.pc = 0x100;
        FlatBus flat(memory);
        std::array<BusSample, 64> samples{};
        TraceBus trace(flat.bus(), samples);
        // Same cached blocks must work with both traced and ordinary buses.
        const auto bus = pass == 0 ? trace.bus() : flat.bus();
        unsigned instructions = 0;
        bool no_fallback = true;
        for (unsigned bound = 0; bound < 100 && state.mode == CpuMode::running; ++bound) {
            const auto result = execute(native, jit, state, bus, 16);
            instructions += result.instructions;
            no_fallback &= result.fallback == JitFallback::none;
        }
        masks[pass] = (state.a == 0x16) | ((state.b == 0) << 1) | ((state.sp == 0xd000) << 2) |
            ((state.pc == 0x10f) << 3) | ((state.mode == CpuMode::halted) << 4) | ((state.f == 0) << 5) |
            ((flat.mcycles() == 34) << 6) | ((instructions == 16) << 7) | (no_fallback << 8);
    }
    return masks[0] | ((masks[1] == 0x1ff) << 9);
}

uint32_t ei_sequence(bool native, Sm83Jit& jit, std::span<uint8_t, 65536> memory) {
    std::fill(memory.begin(), memory.end(), 0);
    constexpr uint8_t program[]{0xfb, 0xfb, 0xf3, 0xfb, 0x00, 0x76};
    constexpr bool ime[]{false, true, false, false, true, true};
    constexpr bool pending[]{true, true, false, true, false, false};
    std::copy(std::begin(program), std::end(program), memory.begin() + 0x200);
    jit.clear();
    Sm83State state; state.pc = 0x200;
    FlatBus flat(memory);
    uint32_t mask = 0;
    for (unsigned i = 0; i < 6; ++i) {
        const auto result = execute(native, jit, state, flat.bus(), 1);
        if (state.ime == ime[i] && state.ei_pending == pending[i] && state.pc == 0x201 + i &&
            flat.mcycles() == i + 1 && result.instructions == 1 && result.fallback == JitFallback::none)
            mask |= 1u << i;
    }
    state = {}; state.pc = 0x200;
    FlatBus whole(memory);
    std::array<BusSample, 16> samples{};
    TraceBus trace(whole.bus(), samples);
    const auto result = execute(native, jit, state, trace.bus(), 16);
    if (state.ime && !state.ei_pending && state.mode == CpuMode::halted && state.pc == 0x206 &&
        result.instructions == 6 && result.fallback == JitFallback::none && whole.mcycles() == 6) mask |= 64;
    bool fetches = trace.size() == 6;
    for (unsigned i = 0; i < 6; ++i)
        fetches &= samples[i].address == 0x200 + i && samples[i].data == program[i] && samples[i].signals == 5;
    if (fetches) mask |= 128;
    return mask;
}

uint32_t wrap_and_lock(bool native, Sm83Jit& jit, std::span<uint8_t, 65536> memory) {
    // Pan Docs: illegal bytes lock the CPU. PC/fetch wrap and ordinary STOP
    // entry use the same CPU-only, no-interrupt contract as the single-step suite.
    constexpr uint8_t illegal[]{0xd3,0xdb,0xdd,0xe3,0xe4,0xeb,0xec,0xed,0xf4,0xfc,0xfd};
    uint32_t mask = 0;
    for (unsigned i = 0; i < 11; ++i) {
        memory[0xffff] = illegal[i]; jit.clear();
        Sm83State state; state.pc = 0xffff;
        FlatBus flat(memory);
        const auto first = execute(native, jit, state, flat.bus(), 1);
        const auto again = execute(native, jit, state, flat.bus(), 1);
        if (state.mode == CpuMode::locked && state.pc == 0 && flat.mcycles() == 1 && first.instructions == 1 &&
            first.fallback == JitFallback::none && again.instructions == 0 && again.mcycles == 0) mask |= 1u << i;
    }
    {
        memory[0xffff] = 0x10; memory[0] = 0x76; jit.clear();
        Sm83State state; state.pc = 0xffff;
        FlatBus flat(memory);
        const auto result = execute(native, jit, state, flat.bus(), 1);
        const auto waited = sm83_wait(state, flat.bus());
        if (state.mode == CpuMode::stopped && state.pc == 1 && flat.mcycles() == 1 && waited == 0 &&
            result.instructions == 1 && result.fallback == JitFallback::none) mask |= 1u << 11;
    }
    {
        memory[0xffff] = 0x01; memory[0] = 0x34; memory[1] = 0x12; jit.clear();
        Sm83State state; state.pc = 0xffff;
        FlatBus flat(memory);
        const auto result = execute(native, jit, state, flat.bus(), 1);
        if (state.bc() == 0x1234 && state.pc == 2 && flat.mcycles() == 3 &&
            result.instructions == 1 && result.fallback == JitFallback::none) mask |= 1u << 12;
    }
    {
        memory[0xffff] = 0xcb; memory[0] = 0x00; jit.clear();
        Sm83State state; state.pc = 0xffff; state.b = 0x80;
        FlatBus flat(memory);
        const auto result = execute(native, jit, state, flat.bus(), 1);
        if (state.b == 1 && state.f == 0x10 && state.pc == 1 && flat.mcycles() == 2 &&
            result.instructions == 1 && result.fallback == JitFallback::none) mask |= 1u << 13;
    }
    return mask;
}
uint32_t reuse(bool native, Sm83Jit& jit, std::span<uint8_t, 65536> memory) {
    jit.clear();
    const auto before = jit.compiled_blocks();
    struct Run { Sm83State state; JitResult result; uint64_t cycles; };
    auto at = [&](uint16_t pc, uint8_t b = 0, unsigned limit = 1) {
        Sm83State state; state.pc = pc; state.a = 0x55; state.b = b;
        FlatBus flat(memory);
        const auto result = execute(native, jit, state, flat.bus(), limit);
        return Run{state, result, flat.mcycles()};
    };
    auto compiled = [&](unsigned count) { return !native || jit.compiled_blocks() - before == count; };
    uint32_t mask = 0;
    memory[0xffff] = 0x3e; memory[0] = 0x12;  // LD A,n across the address wrap.
    auto run = at(0xffff);
    if (run.state.a == 0x12 && run.state.pc == 1 && run.cycles == 2 && compiled(1)) mask |= 1;
    memory[0x200] = 0x3e; memory[0x201] = 0x34;
    run = at(0x200);
    if (run.state.a == 0x34 && run.state.pc == 0x202 && run.cycles == 2 && compiled(1)) mask |= 2;
    memory[0x201] = 0x87;
    run = at(0x200);
    if (run.state.a == 0x87 && compiled(1)) mask |= 4;
    memory[0x200] = 0x06;  // Same PC, different instruction: LD B,n.
    run = at(0x200);
    if (run.state.a == 0x55 && run.state.b == 0x87 && compiled(2)) mask |= 8;
    memory[0x300] = 0xcb; memory[0x301] = 0x00;  // RLC B.
    run = at(0x300, 0x80);
    if (run.state.b == 1 && run.state.f == 0x10 && run.cycles == 2 && compiled(3)) mask |= 16;
    memory[0x301] = 0x08;  // RRC B must not alias RLC B.
    run = at(0x300, 1);
    if (run.state.b == 0x80 && run.state.f == 0x10 && compiled(4)) mask |= 32;
    memory[0x400] = 0xcb; memory[0x401] = 0x08;
    run = at(0x400, 3);
    if (run.state.b == 0x81 && run.state.f == 0x10 && run.state.pc == 0x402 && compiled(4)) mask |= 64;
    memory[0x200] = 0x3e; memory[0x201] = 0x88; memory[0x202] = 0x00; memory[0x203] = 0x76;
    run = at(0x200, 0, 16);  // Multi-instruction identity is a separate space.
    if (run.state.a == 0x88 && run.state.pc == 0x204 && run.state.mode == CpuMode::halted &&
        run.result.instructions == 3 && run.result.fallback == JitFallback::none && compiled(5)) mask |= 128;
    return mask;
}
}  // namespace

bool sm83_blocks(std::array<uint32_t, 8>& results) {
    std::unique_ptr<uint8_t, decltype(&std::free)> memory(static_cast<uint8_t*>(std::calloc(65536, 1)), &std::free);
    auto jit = std::unique_ptr<Sm83Jit>(new (std::nothrow) Sm83Jit);
    if (!memory || !jit || !jit->initialize(std::span<const uint8_t, 65536>(memory.get(), 65536))) return false;
    std::span<uint8_t, 65536> image(memory.get(), 65536);
    for (unsigned native = 0; native < 2; ++native) {
        results[native] = loop(native, *jit, image);
        results[2 + native] = ei_sequence(native, *jit, image);
        results[4 + native] = wrap_and_lock(native, *jit, image);
        results[6 + native] = reuse(native, *jit, image);
    }
    return true;
}
}  // namespace gbx::test
